#pragma once

#include "ECS/Systems.h"
#include "ECS/World.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
struct AiTrace;
}

namespace GameEngine::AIECS
{

class AiRegistry;

/// A headless world that runs only the AI systems, for tests of programs, decisions and determinism.
/// Create the agents in GetWorld(), then call Step. One registry, one seed and one set of agents
/// give the same decisions on every run and with any number of job threads.
class AiTestWorld
{
public:
    /// `jobs` runs the decisions; null runs them on the calling thread. `registry` and `jobs`
    /// must outlive the test world.
    AiTestWorld(AiRegistry& registry, uint64 seed, JobSystem::WorkStealingThreadPool* jobs = nullptr);
    AiTestWorld(const AiTestWorld&) = delete;
    AiTestWorld& operator=(const AiTestWorld&) = delete;

    /// The world the test adds its agents to.
    ECS::World& GetWorld() { return m_World; }
    /// The number of groups that have knowledge in this world.
    uint32 CountGroupKnowledge() const;

    /// Runs `count` AI ticks.
    void Step(uint32 count = 1);

    /// True when the two traces hold the same records, compared byte for byte.
    static bool SameTrace(const Components::AiTrace& left, const Components::AiTrace& right);

    /// Adds the AI systems of `registry` to `systems`, for a test that drives its own schedule.
    static void AddAiSchedule(AiRegistry& registry, ECS::SystemManager& systems);
    /// Updates `world` with `systems` until an AI tick other than `lastTick` runs, and returns that tick.
    /// Use kNoTick before the first tick of the world.
    static uint32 StepSchedule(ECS::SystemManager& systems, ECS::World& world, uint32 lastTick);

    static constexpr uint32 kNoTick = 0xFFFFFFFF;

private:
    ECS::World m_World;
    ECS::SystemManager m_Systems;
    // The AI tick of the last Step; kNoTick before the first.
    uint32 m_LastTick = kNoTick;
};

} // namespace GameEngine::AIECS
