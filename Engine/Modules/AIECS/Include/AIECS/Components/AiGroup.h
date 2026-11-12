#pragma once

#include "GenerationalVector/GenerationalHandle.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

/// A faction, a squad or a herd: the agents that share one knowledge (memory records and plan
/// cells). The world keeps the knowledge and frees it when the AiGroup or its entity goes away.
/// Read it with AIECS::FindGroupRecords and AIECS::FindGroupPlan.
struct AiGroup
{
    /// The number of 4-byte plan cells that the group's agents share. A new cell starts at 0.
    uint32 PlanCells = 0;
    /// The most memory records the group keeps (AI::RecordStore::Capacity).
    uint32 RecordCapacity = 64;
    /// The AI ticks a record stays after its last sighting (AI::RecordStore::ForgetTicks).
    uint32 ForgetTicks = 200;
    /// AI::RecordStore::ClearOnSight.
    bool ClearOnSight = true;
    /// True when the group's sensors write entity handle ids as record subjects and owners. A scene
    /// save then writes them as entity references, and a load points them at the loaded entities.
    bool IdsAreEntities = false;
    /// The group's entry in the knowledge of its world. AiKnowledgeSystem writes it; a copy of
    /// the component gets an entry of its own at its first AI tick.
    GenerationalVector::Handle Knowledge;
};

static_assert(std::is_trivially_copyable_v<AiGroup>);

} // namespace GameEngine::Components
