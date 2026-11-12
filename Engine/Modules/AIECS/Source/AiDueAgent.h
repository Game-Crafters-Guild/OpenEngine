#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::Components
{
struct AiAgent;
struct AiCellsLarge;
struct AiCellsMedium;
struct AiCellsSmall;
struct AiPathTrace;
struct AiStateTree;
struct AiTrace;
}

namespace GameEngine::AIECS
{

/// An agent that decides in this AI tick, with the components AiDecideSystem writes for it. Tree and
/// PathTrace are set only for a state-tree program; the traces are null when the agent has none.
struct AiDueAgent
{
    Components::AiAgent* Agent = nullptr;
    Components::AiStateTree* Tree = nullptr;
    std::span<uint32> Cells;
    Components::AiTrace* Trace = nullptr;
    Components::AiPathTrace* PathTrace = nullptr;
    ECS::EntityHandle Entity;
    uint16 Program = 0;
};

/// The values of the agent's cells component; empty when it has none.
std::span<uint32> SelectCells(Components::AiCellsSmall* small, Components::AiCellsMedium* medium,
                              Components::AiCellsLarge* large);

} // namespace GameEngine::AIECS
