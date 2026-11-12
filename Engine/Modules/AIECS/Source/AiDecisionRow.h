#pragma once

#include "AIECS/AiCommand.h"
#include "AIECS/AiEvent.h"
#include "ECS/ECS.h"
#include "Types/Types.h"

#include <array>

namespace GameEngine::AIECS
{

/// The orders one agent issued in one decision. AiDecideSystem gives each due agent its own row,
/// so the job threads never share one; AiApplySystem reads the rows in entity order.
struct AiDecisionRow
{
    static constexpr uint32 kMaxCommands = 4;
    static constexpr uint32 kMaxRefusedTypes = 4;
    static constexpr uint32 kMaxEvents = 4;

    ECS::EntityHandle Agent;
    uint32 CommandCount = 0;
    std::array<AiCommand, kMaxCommands> Commands{};
    // The first 4 distinct command types that IssueCommand refused in this decision.
    // AiApplySystem logs each type once.
    uint32 RefusedTypeCount = 0;
    std::array<uint16, kMaxRefusedTypes> RefusedTypes{};
    // The events the agent posted; AiApplySystem posts them in entity order.
    uint32 EventCount = 0;
    std::array<AiEvent, kMaxEvents> Events{};
};

} // namespace GameEngine::AIECS
