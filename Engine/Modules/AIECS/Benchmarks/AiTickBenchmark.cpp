// Release timings of the AI decision loop: the consideration rows of OptionSet::Select, an input
// read in order and with cache misses, the tick of 10,000 utility agents and 10,000 state-tree agents at
// load and all due, and the due scan. Prints one line per measurement: the median, minimum and spread of
// kSamples samples. "--arm utility|statetree" prints one path's tick rows; "--workers N" sizes the job pool.

#include "AI/OptionSet.h"
#include "AI/ResponseCurve.h"
#include "AIECS/AiProgram.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiStateTree.h"
#include "ECS/ECSTemplates.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Types/Fnv1a.h"
#include "Types/ParseNumber.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

constexpr uint32 kAgentCount = 10000;
constexpr uint32 kSamples = 15;
constexpr uint32 kWarmUpTicks = 5;
constexpr uint32 kOptionCount = 4;
constexpr uint32 kConsiderationsPerOption = 2;
constexpr uint32 kColumnCount = 4;
constexpr uint32 kSelectRepeats = 50;
// 64 M floats (256 MB): far larger than the last-level cache, so a random read misses.
constexpr uint32 kMissTableSize = 64u * 1024u * 1024u;
// 4 K floats (16 KB): stays in the first-level cache, so the same read chain shows the hash cost.
constexpr uint32 kCachedTableSize = 4u * 1024u;
// About 475 of 10,000 agents due per tick: the stagger spreads them over the cadence (476 or 477).
constexpr uint16 kLoadCadence = 21;
constexpr uint16 kDueScanCadence = 64;
// Work and Rest each run Count, which succeeds on its second tick; Rest also leaves on the High condition.
constexpr const char* kBenchStateTree = R"({
  "root": { "id": 1, "name": "Shift", "children": [
    { "id": 2, "name": "Work", "tasks": ["Count"], "transitions": [ { "to": 3, "on": "completion" } ] },
    { "id": 3, "name": "Rest", "tasks": ["Count"], "transitions": [
      { "to": 2, "on": "condition", "condition": "High" }, { "to": 2, "on": "completion" } ] }
  ] }
})";

// kBenchStateTree where Count also posts a Ping event to its own agent, and Work also leaves on Ping.
constexpr const char* kBenchEventTree = R"({
  "root": { "id": 1, "name": "Shift", "children": [
    { "id": 2, "name": "Work", "tasks": ["CountPing"], "transitions": [
      { "to": 3, "on": "completion" }, { "to": 3, "on": "event", "event": "Ping" } ] },
    { "id": 3, "name": "Rest", "tasks": ["CountPing"], "transitions": [
      { "to": 2, "on": "condition", "condition": "High" }, { "to": 2, "on": "completion" } ] }
  ] }
})";

using Clock = std::chrono::steady_clock;

struct BenchValue
{
    float32 Value = 0.0f;
};

struct SampleStats
{
    float64 Median = 0.0;
    float64 Minimum = 0.0;
    /// The maximum minus the minimum.
    float64 Spread = 0.0;
};

SampleStats Summarize(std::vector<float64> samples)
{
    std::sort(samples.begin(), samples.end());
    return {samples[samples.size() / 2], samples.front(), samples.back() - samples.front()};
}

void PrintStats(const char* label, const SampleStats& stats, const char* unit)
{
    std::printf("%s: median %.3f %s, min %.3f, spread %.3f\n", label, stats.Median, unit, stats.Minimum,
                stats.Spread);
}

float64 ElapsedNanoseconds(Clock::time_point start)
{
    return std::chrono::duration<float64, std::nano>(Clock::now() - start).count();
}

void ReadValue(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
        out[index] = world.GetComponent<BenchValue>(agents[index])->Value;
}

AIECS::AiTaskStatus TickRun(AIECS::AiTaskContext& context)
{
    ++context.GetCells()[0];
    return AIECS::AiTaskStatus::Running;
}

void EnterCount(AIECS::AiTaskContext& context)
{
    context.GetCells()[0] = 0;
}

AIECS::AiTaskStatus TickCount(AIECS::AiTaskContext& context)
{
    return ++context.GetCells()[0] >= 2 ? AIECS::AiTaskStatus::Succeeded : AIECS::AiTaskStatus::Running;
}

uint16 g_PingType = 0;

// Count, and one Ping event to the agent itself at each step.
AIECS::AiTaskStatus TickCountPing(AIECS::AiTaskContext& context)
{
    context.PostEvent({.Type = g_PingType, .Target = context.GetAgent()});
    return TickCount(context);
}

void ReadHigh(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<uint8> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
        out[index] = world.GetComponent<BenchValue>(agents[index])->Value > 0.5f ? 1 : 0;
}

AI::OptionSet MakeOptionSet()
{
    AI::OptionSet set;
    set.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Logistic, 1.0f, 0.5f));
    for (uint16 option = 0; option < kOptionCount; ++option)
    {
        for (uint16 consideration = 0; consideration < kConsiderationsPerOption; ++consideration)
            set.Considerations.push_back({0.0f, 1.0f, static_cast<uint16>((option + consideration) % kColumnCount), 0});
        set.Options.push_back({1.0f, static_cast<uint16>(option * kConsiderationsPerOption),
                               static_cast<uint16>(kConsiderationsPerOption), option, 0});
    }
    set.NoiseMargin = 0.05f;
    return set;
}

// Nanoseconds per consideration and row of Select over kAgentCount rows.
SampleStats MeasureSelect()
{
    const AI::OptionSet set = MakeOptionSet();
    std::vector<float32> columns(kColumnCount * kAgentCount);
    for (uint32 index = 0; index < columns.size(); ++index)
        columns[index] = Hashing::UnitDraw(1, index, 0, 0);
    std::vector<AI::OptionSet::Row> rows(kAgentCount);
    for (uint32 index = 0; index < kAgentCount; ++index)
        rows[index].Agent = index;
    std::vector<uint16> chosen(kAgentCount);
    std::vector<float32> scores(kAgentCount);

    std::vector<float64> samples;
    for (uint32 sample = 0; sample < kSamples; ++sample)
    {
        const Clock::time_point start = Clock::now();
        for (uint32 repeat = 0; repeat < kSelectRepeats; ++repeat)
            set.Select(columns, rows, repeat, 7, chosen, scores);
        samples.push_back(ElapsedNanoseconds(start) /
                          (float64(kSelectRepeats) * kAgentCount * kOptionCount * kConsiderationsPerOption));
    }
    return Summarize(std::move(samples));
}

// Nanoseconds per agent of the registered input, called for kAgentCount agents in entity order.
SampleStats MeasureInputInOrder(AIECS::AiRegistry& registry, uint16 input)
{
    ECS::World world;
    std::vector<ECS::EntityHandle> agents(kAgentCount);
    for (uint32 index = 0; index < kAgentCount; ++index)
        agents[index] = world.Create(BenchValue{Hashing::UnitDraw(3, index, 0, 0)}).GetHandle();
    std::vector<float32> out(kAgentCount);
    const AIECS::AiInputFunction read = registry.GetInput(input);
    std::vector<float64> samples;
    float32 sum = 0.0f;
    for (uint32 sample = 0; sample < kSamples; ++sample)
    {
        const Clock::time_point start = Clock::now();
        read(world, agents, out);
        samples.push_back(ElapsedNanoseconds(start) / float64(kAgentCount));
        sum += out[sample];
    }
    std::printf("  (checksum %f)\n", sum);
    return Summarize(std::move(samples));
}

// Nanoseconds per read of a chain of kAgentCount dependent reads in `table`: the next index hashes
// the value just read, so a read starts when the one before it ends. A fresh seed per sample.
SampleStats MeasureDependentReads(const std::vector<float32>& table)
{
    std::vector<float64> samples;
    float32 sum = 0.0f;
    for (uint32 sample = 0; sample < kSamples; ++sample)
    {
        const uint64 seed = Hashing::SplitMix64(0x5EED0000ull + sample);
        uint64 index = seed % table.size();
        const Clock::time_point start = Clock::now();
        for (uint32 read = 0; read < kAgentCount; ++read)
        {
            const float32 value = table[index];
            sum += value;
            index = Hashing::SplitMix64(seed ^ read ^ std::bit_cast<uint32>(value)) % table.size();
        }
        samples.push_back(ElapsedNanoseconds(start) / float64(kAgentCount));
    }
    std::printf("  (checksum %f)\n", sum);
    return Summarize(std::move(samples));
}

void MeasureReads(AIECS::AiRegistry& registry, uint16 input)
{
    PrintStats("input read in order, through the input", MeasureInputInOrder(registry, input), "ns per agent");
    const std::vector<float32> missTable(kMissTableSize, 0.5f);
    PrintStats("dependent read, 256 MB table (misses)", MeasureDependentReads(missTable), "ns per read");
    const std::vector<float32> cachedTable(kCachedTableSize, 0.5f);
    PrintStats("dependent read, 16 KB table (cached)", MeasureDependentReads(cachedTable), "ns per read");
}

// Milliseconds per AI tick of kAgentCount agents with `program` at `cadence`, decisions on `jobs`
// (null: one thread).
SampleStats MeasureTick(AIECS::AiRegistry& registry, uint16 program, JobSystem::WorkStealingThreadPool* jobs,
                        uint16 cadence)
{
    AIECS::AiTestWorld world(registry, 7, jobs);
    for (uint32 index = 0; index < kAgentCount; ++index)
    {
        Components::AiAgent agent;
        agent.Program = program;
        agent.Cadence = cadence;
        const BenchValue value{Hashing::UnitDraw(2, index, 0, 0)};
        if ((program & Components::AiAgent::kStateTreeProgramBit) != 0)
            world.GetWorld().Create(agent, Components::AiCellsSmall{}, value, Components::AiStateTree{});
        else
            world.GetWorld().Create(agent, Components::AiCellsSmall{}, value);
    }
    world.Step(kWarmUpTicks);
    std::vector<float64> samples;
    for (uint32 sample = 0; sample < kSamples; ++sample)
    {
        const Clock::time_point start = Clock::now();
        world.Step(1);
        samples.push_back(ElapsedNanoseconds(start) * 1.0e-6);
    }
    return Summarize(std::move(samples));
}

// A program whose kColumnCount columns all read `input`, with the TickRun task on every option.
uint16 AddBenchProgram(AIECS::AiRegistry& registry, uint16 input, uint16 task)
{
    AIECS::AiProgram program;
    program.Options = MakeOptionSet();
    program.ColumnInputs.assign(kColumnCount, input);
    for (AI::OptionSet::Option& option : program.Options.Options)
        option.Action = task;
    return registry.AddProgram(std::move(program));
}

// The program of the state-tree file `text`; the registry has the Count and CountPing tasks, the High
// condition and the Ping event.
uint16 AddBenchStateTree(AIECS::AiRegistry& registry, const char* text)
{
    AIECS::AiStateTreeData data;
    std::string error;
    if (!AIECS::ParseAiStateTree(text, data, error))
        std::printf("state tree error: %s\n", error.c_str());
    return registry.CompileStateTree(data);
}

// Which tick rows main prints: all rows, or the tick rows of one path for an A B B A timing run.
enum class BenchArm : uint8
{
    All,
    Utility,
    StateTree,
    Events,
};

struct BenchOptions
{
    BenchArm Arm = BenchArm::All;
    // Empty: the job pool's default worker count.
    std::optional<size_t> Workers;
};

// Prints the arguments that main reads. Returns false, so that a parse failure can return it.
bool PrintUsage()
{
    std::printf("usage: AiTickBenchmark [--arm utility|statetree|events] [--workers N]\n"
                "  --arm      prints only the tick rows of that path (default: all rows)\n"
                "  --workers  the job pool's worker count, at least 1 (default: the processors this process may run on)\n");
    return false;
}

// Reads "--arm utility|statetree" and "--workers N". Prints the usage and returns false for any other argument.
bool ParseOptions(std::span<char* const> args, BenchOptions& options)
{
    for (size_t index = 1; index + 1 < args.size(); index += 2)
    {
        const std::string_view flag = args[index];
        const std::string_view value = args[index + 1];
        const std::optional<uint64> workers = flag == "--workers" ? ParseUnsigned(value) : std::nullopt;
        if (flag == "--arm" && value == "utility")
            options.Arm = BenchArm::Utility;
        else if (flag == "--arm" && value == "statetree")
            options.Arm = BenchArm::StateTree;
        else if (flag == "--arm" && value == "events")
            options.Arm = BenchArm::Events;
        else if (workers.has_value() && *workers > 0)
            options.Workers = static_cast<size_t>(*workers);
        else
            return PrintUsage();
    }
    return args.size() % 2 == 1 || PrintUsage();
}

void MeasureUtilityTicks(AIECS::AiRegistry& registry, uint16 jobProgram, uint16 callingProgram,
                         JobSystem::WorkStealingThreadPool& jobs)
{
    PrintStats("  load, JobThreads input, one thread", MeasureTick(registry, jobProgram, nullptr, kLoadCadence), "ms");
    PrintStats("  load, JobThreads input, all workers", MeasureTick(registry, jobProgram, &jobs, kLoadCadence), "ms");
    PrintStats("  load, CallingThread input, all workers", MeasureTick(registry, callingProgram, &jobs, kLoadCadence),
               "ms");
    PrintStats("  stress, all due, JobThreads input, one thread", MeasureTick(registry, jobProgram, nullptr, 1), "ms");
    PrintStats("  stress, all due, JobThreads input, all workers", MeasureTick(registry, jobProgram, &jobs, 1), "ms");
    PrintStats("  due scan, all workers", MeasureTick(registry, jobProgram, &jobs, kDueScanCadence), "ms");
}

// `label` names the program in the rows: "state tree" or "events".
void MeasureStateTreeTicks(AIECS::AiRegistry& registry, uint16 program, JobSystem::WorkStealingThreadPool& jobs,
                           const std::string& label)
{
    PrintStats(("  load, " + label + ", one thread").c_str(), MeasureTick(registry, program, nullptr, kLoadCadence),
               "ms");
    PrintStats(("  load, " + label + ", all workers").c_str(), MeasureTick(registry, program, &jobs, kLoadCadence),
               "ms");
    PrintStats(("  stress, all due, " + label + ", one thread").c_str(), MeasureTick(registry, program, nullptr, 1),
               "ms");
    PrintStats(("  stress, all due, " + label + ", all workers").c_str(), MeasureTick(registry, program, &jobs, 1),
               "ms");
}

} // namespace

int main(int argc, char** argv)
{
    BenchOptions options;
    if (!ParseOptions({argv, static_cast<size_t>(argc)}, options))
        return 1;
    AIECS::AiRegistry registry;
    const uint16 jobInput = registry.RegisterInput(&ReadValue, AIECS::AiInputThreading::JobThreads);
    const uint16 callingInput = registry.RegisterInput(&ReadValue, AIECS::AiInputThreading::CallingThread);
    const uint16 task = registry.RegisterTask({nullptr, &TickRun, nullptr});
    const uint16 jobProgram = AddBenchProgram(registry, jobInput, task);
    const uint16 callingProgram = AddBenchProgram(registry, callingInput, task);
    registry.RegisterTask({&EnterCount, &TickCount, nullptr, "Count"});
    registry.RegisterTask({&EnterCount, &TickCountPing, nullptr, "CountPing"});
    registry.RegisterCondition(&ReadHigh, "High");
    g_PingType = registry.RegisterEventType("Ping");
    const uint16 stateTreeProgram = AddBenchStateTree(registry, kBenchStateTree);
    const uint16 eventProgram = AddBenchStateTree(registry, kBenchEventTree);

    if (options.Arm == BenchArm::All)
    {
        PrintStats("consideration row in Select", MeasureSelect(), "ns");
        MeasureReads(registry, jobInput);
    }

    std::optional<JobSystem::WorkStealingThreadPool> pool;
    if (options.Workers.has_value())
        pool.emplace(*options.Workers);
    else
        pool.emplace();
    JobSystem::WorkStealingThreadPool& jobs = *pool;
    std::printf("tick of %u agents; load at cadence %u (%u due per tick); due scan at cadence %u; %zu workers:\n",
                kAgentCount, kLoadCadence, kAgentCount / kLoadCadence, kDueScanCadence, jobs.GetWorkerCount());
    if (options.Arm == BenchArm::All || options.Arm == BenchArm::Utility)
        MeasureUtilityTicks(registry, jobProgram, callingProgram, jobs);
    if (options.Arm == BenchArm::All || options.Arm == BenchArm::StateTree)
        MeasureStateTreeTicks(registry, stateTreeProgram, jobs, "state tree");
    if (options.Arm == BenchArm::All || options.Arm == BenchArm::Events)
        MeasureStateTreeTicks(registry, eventProgram, jobs, "events");
    return 0;
}
