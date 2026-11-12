#include "AIECS/AiTestWorld.h"

#include "AIECS/AiGroupKnowledge.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiTrace.h"
#include "AiGroup/AiGroupStore.h"
#include "ECS/SystemScheduling.h"

#include <cassert>
#include <cstring>

namespace GameEngine::AIECS
{

namespace
{

// A step that runs no simulation step leaves the clock as it was; this bounds the retries.
constexpr uint32 kMaxUpdatesPerStep = 1024;

} // namespace

AiTestWorld::AiTestWorld(AiRegistry& registry, uint64 seed, JobSystem::WorkStealingThreadPool* jobs)
    : m_World(jobs)
{
    Components::AiClock clock;
    clock.Seed = seed;
    m_World.SetSingleton(clock);
    RegisterAiWorldHooks(m_World);
    AddAiSchedule(registry, m_Systems);
}

void AiTestWorld::Step(uint32 count)
{
    for (uint32 tick = 0; tick < count; ++tick)
        m_LastTick = StepSchedule(m_Systems, m_World, m_LastTick);
}

void AiTestWorld::AddAiSchedule(AiRegistry& registry, ECS::SystemManager& systems)
{
    ECS::SystemScheduleBuilder builder;
    registry.AddSystemsToSchedule(builder);
    builder.BuildAndRegisterWithWaves(systems, ECS::ScheduleCompleteness::Complete);
}

uint32 AiTestWorld::StepSchedule(ECS::SystemManager& systems, ECS::World& world, uint32 lastTick)
{
    const float32 stepSeconds = static_cast<float32>(world.GetSimulationClock().GetFixedDeltaSeconds());
    for (uint32 update = 0; update < kMaxUpdatesPerStep; ++update)
    {
        systems.Update(world, stepSeconds);
        const Components::AiClock& clock = *world.GetSingleton<Components::AiClock>();
        if (clock.Due && clock.Tick != lastTick)
            return clock.Tick;
    }
    assert(false && "AiTestWorld: the simulation clock does not step");
    return lastTick;
}

uint32 AiTestWorld::CountGroupKnowledge() const
{
    const AiGroupStore* store = AiGroupStore::FindForWorld(m_World.GetWorldId());
    return store != nullptr ? store->GetCount() : 0u;
}

bool AiTestWorld::SameTrace(const Components::AiTrace& left, const Components::AiTrace& right)
{
    return left.Count == right.Count && std::memcmp(left.Records.data(), right.Records.data(), sizeof(left.Records)) == 0;
}

} // namespace GameEngine::AIECS
