#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace GameEngine::Components
{
struct AiPathTrace;
struct AiTrace;
}

namespace GameEngine
{

class EditorDebugServer;

// The AI's live state: get_ai_trace dumps one agent's last decisions from its AiTrace and its AiPathTrace.
void RegisterAiDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine

namespace GameEngine::Editor
{

// The get_ai_trace reply for one agent's AiTrace: recorded (decisions since the trace was added) and
// the newest `maxRecords` records, oldest first. A record holds tick, enteredTick, program, previous
// and chosen (option indices, null when none), score, outcome and the commands the tasks issued.
nlohmann::json BuildAiTraceReport(const Components::AiTrace& trace, std::uint32_t maxRecords);

// The get_ai_trace reply part for a state-tree agent's AiPathTrace: pathRecorded (decisions since the trace was
// added) and pathRecords, the newest `maxRecords` records, oldest first. A record holds tick, program, result
// (running, succeeded or failed) and states (the active state indices from the root down).
nlohmann::json BuildAiPathTraceReport(const Components::AiPathTrace& trace, std::uint32_t maxRecords);

// The get_ai_trace `ticks` parameter: AiTrace::kCapacity when absent, the value when it is a whole
// number from 1 to AiTrace::kCapacity, and std::nullopt for any other value.
std::optional<std::uint32_t> ReadAiTraceTicks(const nlohmann::json& params);

} // namespace GameEngine::Editor
