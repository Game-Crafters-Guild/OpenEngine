#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <array>

namespace GameEngine::AIECS
{

/// What the chosen option's task did on the decision tick.
enum class AiTraceOutcome : uint8
{
    /// No option scored above 0, so no task ran.
    None,
    Running,
    Succeeded,
    Failed,
};

/// One decision of one agent. The record has no padding, so two traces compare byte for byte.
struct AiTraceRecord
{
    static constexpr uint16 kNoOption = 0xFFFF;
    /// The orders the record keeps; CommandCount can be larger.
    static constexpr uint32 kMaxCommands = 2;

    /// The AI tick of the decision.
    uint32 Tick = 0;
    /// The AI tick the chosen option started.
    uint32 EnteredTick = 0;
    /// The chosen option's score.
    float32 Score = 0.0f;
    uint16 Program = 0;
    /// The active option before the decision, or kNoOption.
    uint16 Previous = kNoOption;
    /// The option the decision chose, or kNoOption.
    uint16 Chosen = kNoOption;
    AiTraceOutcome Outcome = AiTraceOutcome::None;
    /// The orders the tasks issued in the decision (Enter, Tick and Exit).
    uint8 CommandCount = 0;
    std::array<ECS::EntityHandle, kMaxCommands> CommandTargets{};
    std::array<int32, kMaxCommands> CommandArguments{};
    std::array<uint16, kMaxCommands> CommandTypes{};
};

static_assert(sizeof(AiTraceRecord) == 40, "AiTraceRecord must have no padding");

} // namespace GameEngine::AIECS
