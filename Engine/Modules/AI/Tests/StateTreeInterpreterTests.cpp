// The state tree: the four child policies, the order of the checks, the exit reasons, timeouts in ticks,
// the together joins, the modifiers, the reload by stable id and the determinism rule.
#include "AI/StateTree/StateTreeCompileError.h"
#include "AI/StateTree/StateTreeDefinition.h"
#include "AI/StateTree/StateTreeHost.h"
#include "AI/StateTree/StateTreeInterpreter.h"
#include "AI/StateTree/StateTreeProgram.h"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AI;

namespace
{

using Definition = StateTreeDefinition;
using Status = StateTreeTaskStatus;

// Task kind k returns Results[k]; condition c reads Conditions[c]; column c of a Utility state reads Columns[c].
// Each enter and exit is written to Log as "+k" or "-k:reason".
struct TestHost
{
    std::vector<Status> Results = std::vector<Status>(8, Status::Running);
    std::vector<bool> Conditions = std::vector<bool>(8, false);
    std::vector<float32> Columns;
    std::vector<std::string> Log;

    static TestHost& From(void* user) { return *static_cast<TestHost*>(user); }
    static std::string Kind(const StateTreeProgram& program, uint32 task)
    {
        return std::to_string(program.GetTasks()[task].Kind);
    }
    static void Enter(void* user, const StateTreeProgram& program, uint32 task)
    {
        From(user).Log.push_back("+" + Kind(program, task));
    }
    static Status Tick(void* user, const StateTreeProgram& program, uint32 task)
    {
        return From(user).Results[program.GetTasks()[task].Kind];
    }
    static void Exit(void* user, const StateTreeProgram& program, uint32 task, StateTreeExitReason reason)
    {
        constexpr const char* kReasons[] = {"Completed", "Failed", "Preempted", "Reloaded"};
        From(user).Log.push_back("-" + Kind(program, task) + ":" + kReasons[static_cast<int>(reason)]);
    }
    static bool Test(void* user, uint16 condition) { return From(user).Conditions[condition]; }
    static void Fill(void* user, uint16, std::span<float32> columns)
    {
        for (size_t column = 0; column < columns.size(); ++column)
            columns[column] = From(user).Columns[column];
    }

    StateTreeHost Bind() { return {this, &Enter, &Tick, &Exit, &Test, &Fill}; }
};

Definition::State MakeState(uint32 id, uint32 parent, std::vector<uint16> tasks = {},
                            StateTreePolicy policy = StateTreePolicy::First)
{
    Definition::State state;
    state.Id = id;
    state.ParentId = parent;
    state.Tasks = std::move(tasks);
    state.Policy = policy;
    return state;
}

Definition::Transition OnCondition(uint16 condition, uint32 target)
{
    return {StateTreeTrigger::Condition, StateTreeCompletion::Any, condition, 0, target};
}

StateTreeProgram Compile(const Definition& definition)
{
    StateTreeProgram program;
    StateTreeCompileError error;
    EXPECT_TRUE(StateTreeProgram::Compile(definition, program, error)) << error.Message;
    return program;
}

// One agent: its state, its cooldown cells and an interpreter bound to them.
struct TestAgent
{
    StateTreeState State;
    std::array<uint32, 8> ReadyTicks{};
    StateTreeInterpreter Interpreter;

    void Start(const StateTreeProgram& program, const StateTreeHost& host, uint64 agent, uint64 seed, uint32 tick)
    {
        Interpreter.Bind(State, ReadyTicks, agent, seed);
        Interpreter.Start(program, host, tick);
    }
    void Step(const StateTreeProgram& program, const StateTreeHost& host, uint32 tick,
              std::span<const uint16> events = {})
    {
        Interpreter.Step(program, host, events, tick);
    }
    bool Remap(const StateTreeProgram& oldProgram, const StateTreeProgram& newProgram, const StateTreeHost& host,
               uint32 tick)
    {
        return Interpreter.Remap(oldProgram, newProgram, host, tick);
    }
    std::span<const StateTreeInterpreter::Frame> GetActivePath() const { return Interpreter.GetActivePath(); }
    Status GetResult() const { return Interpreter.GetResult(); }
};

// The stable ids of the active path, from the root down.
std::vector<uint32> PathIds(const TestAgent& interpreter, const StateTreeProgram& program)
{
    std::vector<uint32> ids;
    for (const StateTreeInterpreter::Frame& frame : interpreter.GetActivePath())
        ids.push_back(program.GetStates()[frame.State].StableId);
    return ids;
}

// One option per child that reads column i linearly.
OptionSet OneColumnPerChild(uint16 children)
{
    OptionSet options;
    options.Curves.push_back(ResponseCurve::FromPreset(CurvePreset::Linear, 1.0f, 0.5f));
    for (uint16 child = 0; child < children; ++child)
    {
        options.Considerations.push_back({0.0f, 1.0f, child, 0});
        options.Options.push_back({1.0f, child, 1});
    }
    return options;
}

// One step of a root with tasks 1 and 2 under `join`; task 1 runs, task 2 returns `second`.
std::pair<Status, std::vector<std::string>> RunTogether(StateTreeJoin join, Status second)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent, {1, 2})};
    definition.States[0].Join = join;
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    host.Results[2] = second;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);
    interpreter.Step(program, host.Bind(), 1);
    return {interpreter.GetResult(), host.Log};
}

// True when the compiler refuses the definition and states a reason.
bool CompileRefuses(const Definition& definition)
{
    StateTreeProgram program;
    StateTreeCompileError error;
    return !StateTreeProgram::Compile(definition, program, error) && !error.Message.empty();
}

} // namespace

TEST(StateTreeInterpreter, FirstEntersTheFirstChildWhoseConditionPasses)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 1, {3})};
    definition.States[1].EnterCondition = 0;
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;

    interpreter.Start(program, host.Bind(), 1, 1, 0);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3}));

    host.Conditions[0] = true;
    interpreter.Start(program, host.Bind(), 1, 1, 0);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2}));
}

TEST(StateTreeInterpreter, SequenceRunsEachChildAfterThePreviousSucceedsThenCompletes)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent, {}, StateTreePolicy::Sequence), MakeState(2, 1, {2}),
                         MakeState(3, 1, {3})};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2}));

    host.Results[2] = Status::Succeeded;
    interpreter.Step(program, host.Bind(), 1);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3}));

    host.Results[3] = Status::Succeeded;
    interpreter.Step(program, host.Bind(), 2);
    EXPECT_TRUE(interpreter.GetActivePath().empty());
    EXPECT_EQ(interpreter.GetResult(), Status::Succeeded);
}

TEST(StateTreeInterpreter, RandomDrawIsSeededAndNeverTakesAZeroWeight)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent, {}, StateTreePolicy::Random), MakeState(2, 1),
                         MakeState(3, 1), MakeState(4, 1)};
    definition.States[2].Weight = 0.0f;
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    std::vector<int> counts(5, 0);
    for (uint64 agent = 0; agent < 200; ++agent)
    {
        TestAgent first;
        TestAgent second;
        first.Start(program, host.Bind(), agent, 7, 3);
        second.Start(program, host.Bind(), agent, 7, 3);
        const uint32 child = PathIds(first, program)[1];
        EXPECT_EQ(child, PathIds(second, program)[1]);
        ++counts[child];
    }
    EXPECT_EQ(counts[3], 0);
    EXPECT_GT(counts[2], 60);
    EXPECT_GT(counts[4], 60);
}

TEST(StateTreeInterpreter, UtilityTakesTheBestChildAndHoldsItWithinTheCommitmentMargin)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent, {}, StateTreePolicy::Utility), MakeState(2, 1, {2}),
                         MakeState(3, 1, {3})};
    definition.States[0].Utility = OneColumnPerChild(2);
    definition.States[0].Utility.CommitmentMargin = 0.2f;
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    host.Columns = {0.3f, 0.6f};
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3}));

    host.Columns = {0.75f, 0.6f};
    interpreter.Step(program, host.Bind(), 1);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3}));

    host.Columns = {0.9f, 0.6f};
    host.Log.clear();
    interpreter.Step(program, host.Bind(), 2);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2}));
    EXPECT_EQ(host.Log, (std::vector<std::string>{"-3:Preempted", "+2"}));
}

TEST(StateTreeInterpreter, AShallowerTransitionPreemptsADeeperOne)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1), MakeState(5, 1, {5}),
                         MakeState(3, 2, {3}), MakeState(4, 2, {4})};
    definition.States[1].Transitions = {OnCondition(0, 5)};
    definition.States[3].Transitions = {OnCondition(1, 4)};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2, 3}));

    host.Conditions = {true, true};
    interpreter.Step(program, host.Bind(), 1);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 5}));
}

TEST(StateTreeInterpreter, EachExitGivesItsReason)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2, 3}), MakeState(4, 1, {4})};
    definition.States[1].Transitions = {OnCondition(0, 4)};
    definition.States[2].Transitions = {{StateTreeTrigger::Completion, StateTreeCompletion::Failed, 0, 0, 2}};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);

    host.Results[2] = Status::Succeeded;
    interpreter.Step(program, host.Bind(), 1);
    host.Conditions[0] = true;
    interpreter.Step(program, host.Bind(), 2);
    host.Conditions[0] = false;
    host.Results[4] = Status::Failed;
    interpreter.Step(program, host.Bind(), 3);

    EXPECT_EQ(host.Log, (std::vector<std::string>{"+2", "-2:Completed", "+3", "-3:Preempted", "+4", "-4:Failed",
                                                  "+2"}));
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2}));
}

TEST(StateTreeInterpreter, ATimeoutFiresOnItsTickCount)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 1, {3})};
    definition.States[1].Transitions = {{StateTreeTrigger::Timeout, StateTreeCompletion::Any, kStateTreeNone, 5, 3}};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 10);
    for (uint32 tick = 11; tick < 15; ++tick)
        interpreter.Step(program, host.Bind(), tick);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2}));
    interpreter.Step(program, host.Bind(), 15);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3}));
}

TEST(StateTreeInterpreter, AnEventFiresOnTheStepThatHoldsIt)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 1, {3})};
    definition.States[1].Transitions = {
        {StateTreeTrigger::Event, StateTreeCompletion::Any, kStateTreeNone, 0, 3, 1}};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);
    const std::array<uint16, 2> otherEvents{0, 2};
    interpreter.Step(program, host.Bind(), 1, otherEvents);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2}));
    const std::array<uint16, 1> itsEvent{1};
    interpreter.Step(program, host.Bind(), 2, itsEvent);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3}));
}

TEST(StateTreeInterpreter, TogetherJoinsDecideByTheirRule)
{
    EXPECT_EQ(RunTogether(StateTreeJoin::Any, Status::Succeeded),
              std::make_pair(Status::Succeeded, std::vector<std::string>{"+1", "+2", "-2:Completed", "-1:Preempted"}));
    EXPECT_EQ(RunTogether(StateTreeJoin::All, Status::Succeeded).first, Status::Running);
    EXPECT_EQ(RunTogether(StateTreeJoin::All, Status::Failed).first, Status::Failed);
    EXPECT_EQ(RunTogether(StateTreeJoin::Primary, Status::Failed).first, Status::Running);
}

TEST(StateTreeInterpreter, ModifiersAbortCoolDownAndRepeat)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 1, {3})};
    definition.States[1].Modifiers = {{StateTreeModifierKind::AbortIf, 0, 0},
                                      {StateTreeModifierKind::Cooldown, kStateTreeNone, 10},
                                      {StateTreeModifierKind::Repeat, kStateTreeNone, 1}};
    definition.States[1].Transitions = {{StateTreeTrigger::Completion, StateTreeCompletion::Any, 0, 0, 1}};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);

    host.Results[2] = Status::Succeeded;
    interpreter.Step(program, host.Bind(), 1);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 2})) << "Repeat runs the state once more";
    host.Results[2] = Status::Running;
    host.Conditions[0] = true;
    interpreter.Step(program, host.Bind(), 2);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3})) << "AbortIf fails it; the cooldown holds";
    EXPECT_EQ(host.Log.back(), "+3");
}

TEST(StateTreeInterpreter, ACooldownNearTheLastTickHoldsUntilTheLastTick)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 1, {3})};
    definition.States[1].Modifiers = {{StateTreeModifierKind::Cooldown, kStateTreeNone, 0xFFFFFFF0u}};
    definition.States[1].Transitions = {{StateTreeTrigger::Completion, StateTreeCompletion::Any, 0, 0, 1}};
    const StateTreeProgram program = Compile(definition);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(program, host.Bind(), 1, 1, 0);

    host.Results[2] = Status::Succeeded;
    interpreter.Step(program, host.Bind(), 100);
    EXPECT_EQ(PathIds(interpreter, program), (std::vector<uint32>{1, 3})) << "the cooldown does not wrap";
}

TEST(StateTreeInterpreter, AReloadKeepsTheStatesWhoseStableIdsRemain)
{
    Definition before;
    before.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 2, {3})};
    Definition after;
    after.States = {MakeState(1, Definition::kNoParent), MakeState(9, 1), MakeState(2, 1, {2}), MakeState(4, 2, {4})};
    const StateTreeProgram oldProgram = Compile(before);
    const StateTreeProgram newProgram = Compile(after);
    TestHost host;
    TestAgent interpreter;
    interpreter.Start(oldProgram, host.Bind(), 1, 1, 0);
    EXPECT_EQ(PathIds(interpreter, oldProgram), (std::vector<uint32>{1, 2, 3}));

    host.Log.clear();
    interpreter.Remap(oldProgram, newProgram, host.Bind(), 5);
    EXPECT_EQ(PathIds(interpreter, newProgram), (std::vector<uint32>{1, 2, 4}));
    EXPECT_EQ(host.Log, (std::vector<std::string>{"-3:Reloaded", "+4"}));
    EXPECT_EQ(interpreter.GetActivePath()[1].EnteredTick, 0u) << "the kept state keeps its time";
}

TEST(StateTreeInterpreter, StopAndRemapRefuseAPathOfAnotherProgram)
{
    Definition deep;
    deep.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1, {2}), MakeState(3, 2, {3})};
    Definition flat;
    flat.States = {MakeState(1, Definition::kNoParent, {5})};
    const StateTreeProgram deepProgram = Compile(deep);
    const StateTreeProgram flatProgram = Compile(flat);
    TestHost host;
    TestAgent stopped;
    stopped.Start(deepProgram, host.Bind(), 1, 1, 0);
    TestAgent remapped;
    remapped.Start(deepProgram, host.Bind(), 1, 1, 0);

    // The paths hold states 1 and 2, which the flat program does not have.
    host.Log.clear();
    EXPECT_FALSE(stopped.Interpreter.Stop(flatProgram, host.Bind()));
    EXPECT_TRUE(stopped.GetActivePath().empty());
    EXPECT_FALSE(remapped.Remap(flatProgram, flatProgram, host.Bind(), 4));
    EXPECT_EQ(PathIds(remapped, flatProgram), (std::vector<uint32>{1}));
    EXPECT_EQ(host.Log, (std::vector<std::string>{"+5"})) << "no task of the path exits";
}

TEST(StateTreeInterpreter, TwoInterpretersWithOneProgramAndOneInputTakeOnePath)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent, {}, StateTreePolicy::Random), MakeState(2, 1, {2}),
                         MakeState(3, 1, {}, StateTreePolicy::Utility), MakeState(4, 3, {4}), MakeState(5, 3, {5})};
    definition.States[1].Transitions = {{StateTreeTrigger::Timeout, StateTreeCompletion::Any, kStateTreeNone, 7, 1}};
    definition.States[2].Transitions = {OnCondition(0, 1)};
    definition.States[2].Utility = OneColumnPerChild(2);
    const StateTreeProgram program = Compile(definition);
    TestHost first;
    first.Columns = {0.5f, 0.5f};
    TestHost second = first;
    TestAgent a;
    TestAgent b;
    a.Start(program, first.Bind(), 42, 9, 0);
    b.Start(program, second.Bind(), 42, 9, 0);
    // The leaf at each of the first ticks for seed 42 and agent 9; it changes when the draw keying changes.
    constexpr uint32 kGoldenTicks = 12;
    const std::vector<uint32> golden{5, 5, 4, 4, 5, 4, 4, 5, 4, 5, 2, 2};
    std::vector<uint32> leaves;
    for (uint32 tick = 1; tick < 500; ++tick)
    {
        for (TestHost* host : {&first, &second})
        {
            host->Conditions[0] = tick % 11 == 0;
            host->Columns = {static_cast<float32>(tick % 5) / 4.0f, static_cast<float32>(tick % 3) / 2.0f};
        }
        a.Step(program, first.Bind(), tick);
        b.Step(program, second.Bind(), tick);
        ASSERT_EQ(PathIds(a, program), PathIds(b, program)) << "tick " << tick;
        if (tick <= kGoldenTicks)
            leaves.push_back(PathIds(a, program).back());
    }
    EXPECT_EQ(a.GetResult(), Status::Running) << "the paths stay live for every tick";
    EXPECT_EQ(first.Log, second.Log);
    EXPECT_EQ(leaves, golden);
}

TEST(StateTreeProgram, CompileRefusesAProgramItCannotRun)
{
    Definition twoRoots;
    twoRoots.States = {MakeState(1, Definition::kNoParent), MakeState(2, Definition::kNoParent)};
    Definition loop;
    loop.States = {MakeState(1, Definition::kNoParent), MakeState(2, 3), MakeState(3, 2)};
    Definition badTarget;
    badTarget.States = {MakeState(1, Definition::kNoParent)};
    badTarget.States[0].Transitions = {OnCondition(0, 8)};
    Definition utilityMismatch;
    utilityMismatch.States = {MakeState(1, Definition::kNoParent, {}, StateTreePolicy::Utility), MakeState(2, 1)};
    Definition tooDeep;
    for (uint32 id = 1; id <= StateTreeProgram::kMaxDepth + 1; ++id)
        tooDeep.States.push_back(MakeState(id, id == 1 ? Definition::kNoParent : id - 1));
    EXPECT_TRUE(CompileRefuses(Definition{}));
    EXPECT_TRUE(CompileRefuses(twoRoots));
    EXPECT_TRUE(CompileRefuses(loop));
    EXPECT_TRUE(CompileRefuses(badTarget));
    EXPECT_TRUE(CompileRefuses(utilityMismatch));
    EXPECT_TRUE(CompileRefuses(tooDeep));
    Definition noEvent;
    noEvent.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1)};
    noEvent.States[1].Transitions = {{StateTreeTrigger::Event, StateTreeCompletion::Any, kStateTreeNone, 0, 1}};
    EXPECT_TRUE(CompileRefuses(noEvent));
}

TEST(StateTreeProgram, CompileRefusesSpansPastTheSixteenBitLimit)
{
    Definition definition;
    definition.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1)};
    for (Definition::State& state : definition.States)
        state.Transitions.assign(kStateTreeNone / 2 + 1, OnCondition(0, 1));
    StateTreeProgram program;
    StateTreeCompileError error;
    EXPECT_FALSE(StateTreeProgram::Compile(definition, program, error));
    EXPECT_NE(error.Message.find("65536 transitions; the limit is 65534"), std::string::npos) << error.Message;
    EXPECT_EQ(error.StateId, StateTreeCompileError::kNoState);
    EXPECT_EQ(error.Item, StateTreeCompileItem::Program);
}

// A refusal names the state id and the item, so an editor can mark the item that has the fault.
TEST(StateTreeProgram, ARefusalNamesTheStateAndTheItem)
{
    Definition badTarget;
    badTarget.States = {MakeState(1, Definition::kNoParent), MakeState(2, 1)};
    badTarget.States[1].Transitions = {OnCondition(0, 1), OnCondition(0, 8)};
    Definition noAbortCondition;
    noAbortCondition.States = {MakeState(1, Definition::kNoParent), MakeState(4, 1)};
    noAbortCondition.States[1].Modifiers = {{StateTreeModifierKind::Cooldown, kStateTreeNone, 5},
                                            {StateTreeModifierKind::AbortIf, kStateTreeNone, 0}};
    Definition missingOption;
    missingOption.States = {MakeState(1, Definition::kNoParent, {}, StateTreePolicy::Utility), MakeState(2, 1),
                            MakeState(3, 1)};
    missingOption.States[0].Utility.Options.resize(1);

    StateTreeProgram program;
    StateTreeCompileError error;
    ASSERT_FALSE(StateTreeProgram::Compile(badTarget, program, error));
    EXPECT_EQ(error.StateId, 2u);
    EXPECT_EQ(error.Item, StateTreeCompileItem::Transition);
    EXPECT_EQ(error.ItemIndex, 1);
    EXPECT_NE(error.Message.find("Use the id of a state in the program."), std::string::npos) << error.Message;

    ASSERT_FALSE(StateTreeProgram::Compile(noAbortCondition, program, error));
    EXPECT_EQ(error.StateId, 4u);
    EXPECT_EQ(error.Item, StateTreeCompileItem::Modifier);
    EXPECT_EQ(error.ItemIndex, 1);

    ASSERT_FALSE(StateTreeProgram::Compile(missingOption, program, error));
    EXPECT_EQ(error.StateId, 1u);
    EXPECT_EQ(error.Item, StateTreeCompileItem::Option);
    EXPECT_EQ(error.ItemIndex, 1);
}
