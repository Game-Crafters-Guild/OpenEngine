#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::AI
{
class RecordStore;
}

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::AIECS
{

/// Registers the hook that frees the knowledge of a group when its AiGroup or its entity goes
/// away. Every world that runs the AI systems needs it; call it once, when the world is created.
void RegisterAiWorldHooks(ECS::World& world);

/// The memory records of the group entity, or null when it has no AiGroup or no AI tick yet; valid
/// until the next AI tick or entity removal. Call it on the main thread while no AI system runs; each
/// call takes a lock. A task calls AiTaskContext::FindGroupRecords.
const AI::RecordStore* FindGroupRecords(const ECS::World& world, ECS::EntityHandle group);

/// The plan cells of the group entity: 4-byte cells that its agents share, for example the gold
/// a build reserved. Empty when FindGroupRecords gives null. Valid as long as that pointer.
/// Call it on the main thread while no AI system runs; each call takes a lock, as FindGroupRecords does.
std::span<uint32> FindGroupPlan(ECS::World& world, ECS::EntityHandle group);

} // namespace GameEngine::AIECS
