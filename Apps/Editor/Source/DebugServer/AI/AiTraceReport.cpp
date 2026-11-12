#include "DebugServer/AI/AiDebugHandlers.h"

#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiTrace.h"

#include <algorithm>
#include <string_view>

namespace GameEngine::Editor
{

namespace
{

using json = nlohmann::json;

std::string_view OutcomeName(AIECS::AiTraceOutcome outcome)
{
    switch (outcome)
    {
        case AIECS::AiTraceOutcome::None:
            return "none";
        case AIECS::AiTraceOutcome::Running:
            return "running";
        case AIECS::AiTraceOutcome::Succeeded:
            return "succeeded";
        case AIECS::AiTraceOutcome::Failed:
            return "failed";
    }
    return "none";
}

std::string_view ResultName(AI::StateTreeTaskStatus result)
{
    switch (result)
    {
        case AI::StateTreeTaskStatus::Running:
            return "running";
        case AI::StateTreeTaskStatus::Succeeded:
            return "succeeded";
        case AI::StateTreeTaskStatus::Failed:
            return "failed";
    }
    return "running";
}

json OptionIndex(uint16 option)
{
    return option == AIECS::AiTraceRecord::kNoOption ? json(nullptr) : json(option);
}

json RecordJson(const AIECS::AiTraceRecord& record)
{
    json commands = json::array();
    const uint32 kept = std::min<uint32>(record.CommandCount, AIECS::AiTraceRecord::kMaxCommands);
    for (uint32 index = 0; index < kept; ++index)
    {
        commands.push_back({{"type", record.CommandTypes[index]},
                            {"targetEntityId", record.CommandTargets[index].id},
                            {"argument", record.CommandArguments[index]}});
    }
    return {{"tick", record.Tick},
            {"enteredTick", record.EnteredTick},
            {"program", record.Program},
            {"previous", OptionIndex(record.Previous)},
            {"chosen", OptionIndex(record.Chosen)},
            {"score", record.Score},
            {"outcome", OutcomeName(record.Outcome)},
            {"commandCount", record.CommandCount},
            {"commands", std::move(commands)}};
}

json PathRecordJson(const AIECS::AiPathTraceRecord& record)
{
    json states = json::array();
    for (uint8 depth = 0; depth < record.Depth; ++depth)
        states.push_back(record.States[depth]);
    return {{"tick", record.Tick},
            {"program", record.Program},
            {"result", ResultName(record.Result)},
            {"states", std::move(states)}};
}

// The record number of the oldest record shown: a ring of `capacity` records that recorded `count` shows its
// newest `maxRecords`, oldest first.
uint32 FirstShownRecord(uint32 count, uint32 capacity, uint32 maxRecords)
{
    return count - std::min({count, capacity, maxRecords});
}

} // namespace

json BuildAiTraceReport(const Components::AiTrace& trace, std::uint32_t maxRecords)
{
    json records = json::array();
    for (uint32 record = FirstShownRecord(trace.Count, Components::AiTrace::kCapacity, maxRecords);
         record < trace.Count; ++record)
        records.push_back(RecordJson(trace.Records[record % Components::AiTrace::kCapacity]));
    return {{"recorded", trace.Count}, {"records", std::move(records)}};
}

json BuildAiPathTraceReport(const Components::AiPathTrace& trace, std::uint32_t maxRecords)
{
    json records = json::array();
    for (uint32 record = FirstShownRecord(trace.Count, Components::AiPathTrace::kCapacity, maxRecords);
         record < trace.Count; ++record)
        records.push_back(PathRecordJson(trace.Records[record % Components::AiPathTrace::kCapacity]));
    return {{"pathRecorded", trace.Count}, {"pathRecords", std::move(records)}};
}

std::optional<std::uint32_t> ReadAiTraceTicks(const json& params)
{
    if (!params.contains("ticks"))
        return Components::AiTrace::kCapacity;
    const json& ticks = params["ticks"];
    if (!ticks.is_number_unsigned())
        return std::nullopt;
    const uint64 value = ticks.get<uint64>();
    if (value == 0 || value > Components::AiTrace::kCapacity)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

} // namespace GameEngine::Editor
