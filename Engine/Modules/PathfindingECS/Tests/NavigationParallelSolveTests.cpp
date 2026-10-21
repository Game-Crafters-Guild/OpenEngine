// NavigationPathfindingSystem's grid solves run on the world's job pool inside
// one Update: the result of every request, its path handle and the order the
// results are applied in do not depend on the pool, and nothing may write a
// grid while the solves read it.
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "NavigationTestSurfaces.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "Components/Transform.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/PathfindingTypes.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

constexpr float32 kDt = 1.0f / 60.0f;
constexpr uint32 kPoolWorkers = 16;

// One frame of one agent: every field of its state and the points of the path
// it holds. NavigationAgentState has padding, so it is compared field by field.
struct AgentRecord
{
    std::tuple<float32, float32, float32, bool, bool, bool, bool, uint32, uint32, uint32, float32, float32, float32,
               float32, float32, float32, float32, float32, float32, uint8, int32, float32, uint32>
        State;
    std::vector<Pathfinding::PathPoint> Points;

    bool operator==(const AgentRecord& other) const
    {
        if (State != other.State || Points.size() != other.Points.size())
            return false;
        for (std::size_t i = 0; i < Points.size(); ++i)
        {
            if (Points[i].X != other.Points[i].X || Points[i].Y != other.Points[i].Y ||
                Points[i].Z != other.Points[i].Z)
                return false;
        }
        return true;
    }
};

AgentRecord Record(const Components::NavigationAgentState& s, const Pathfinding::PathBuffer& buffer)
{
    AgentRecord record;
    record.State = {s.DestinationX, s.DestinationY, s.DestinationZ, s.HasDestination, s.PathDirty,
                    s.ReplanRequested, s.HasCellReservations, s.PathIndex, s.PathGeneration,
                    s.CurrentPathPointIndex, s.CurrentWaypointX, s.CurrentWaypointY, s.CurrentWaypointZ,
                    s.NextWaypointX, s.NextWaypointY, s.NextWaypointZ, s.VelocityX, s.VelocityY, s.VelocityZ,
                    s.Status, s.CrowdAgentHandle, s.ReplanTimer, s.PathRequestSequence};
    const Pathfinding::PathHandle path{s.PathIndex, s.PathGeneration};
    record.Points.resize(buffer.GetPointCount(path));
    buffer.CopyPoints(path, record.Points.data(), static_cast<uint32>(record.Points.size()));
    return record;
}

// What the claim-helper hook reads and does. The hook is a plain function
// pointer, so its inputs are file-scope.
std::atomic<uint32> s_HelpersSeenWhileSolving{0};
std::atomic<uint32> s_HelpersSeenOutsideSolving{0};
std::atomic<Pathfinding::GridMap*> s_GridToWrite{nullptr};
Pathfinding::NavMapHandle s_MapToRemove{};
// Set by the first helper that writes, so exactly one thread asserts: several
// threadsafe death-test asserts at once interleave their messages.
std::atomic<bool> s_WriteFired{false};

class NavigationParallelSolveTest : public ::testing::Test
{
protected:
    void SetUp() override { NavigationService::Initialize(); }

    void TearDown() override
    {
        m_World.reset();
        m_Pool.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    // A world on a pool of `workers` threads, or on no pool for 0.
    void CreateWorld(uint32 workers)
    {
        if (workers > 0)
            m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(workers);
        m_World = std::make_unique<ECS::World>(m_Pool.get());
    }

    Pathfinding::NavMapHandle CreateGrid(uint32 width, uint32 depth)
    {
        auto entity = Testing::CreateFlatGridSurface(*m_World, width, depth, 1.0f);
        return Testing::SurfaceMap(*m_World, entity);
    }

    ECS::Entity CreateAgent(Pathfinding::NavMapHandle map, float32 startX, float32 startZ, float32 destX,
                            float32 destZ)
    {
        auto entity = m_World->Create();

        Components::NavigationAgent agent{};
        agent.Speed = 3.5f;
        agent.Acceleration = 100.0f;
        agent.StoppingDistance = 0.3f;
        agent.Radius = 0.25f;
        agent.NavMapIndex = map.Index;
        agent.NavMapGeneration = map.Generation;
        agent.AgentId = entity.GetHandle().id;
        agent.ReplanInterval = 0.0f;
        entity.Set(agent);

        Components::NavigationAgentState state{};
        state.DestinationX = destX;
        state.DestinationZ = destZ;
        state.HasDestination = true;
        state.PathDirty = true;
        entity.Set(state);

        Components::Transform xf{};
        xf.matrix[0] = 1.0f; xf.matrix[5] = 1.0f; xf.matrix[10] = 1.0f; xf.matrix[15] = 1.0f;
        xf.matrix[12] = startX;
        xf.matrix[14] = startZ;
        entity.Set(xf);

        Components::WorldTransform wt{};
        wt.matrix[0] = 1.0f; wt.matrix[5] = 1.0f; wt.matrix[10] = 1.0f; wt.matrix[15] = 1.0f;
        wt.matrix[12] = startX;
        wt.matrix[14] = startZ;
        entity.Set(wt);

        m_World->ProcessCommands();
        return entity;
    }

    // A wall across the middle of the grid with two gaps, so routes are long
    // and agents converge on the gaps, where the movement system's corridor
    // checks re-dirty them.
    void BuildWall(Pathfinding::NavMapHandle map, uint32 depth, uint32 wallX)
    {
        auto* grid = NavigationService::Get().GetGridMap(map);
        ASSERT_NE(grid, nullptr);
        for (uint32 z = 0; z < depth; ++z)
        {
            if (z != depth / 4 && z != 3 * depth / 4)
                grid->SetCellBlocked(wallX, z, true);
        }
    }

    // Agents on the left of the wall heading to mirrored cells on its right,
    // stepped through pathfinding and movement; returns every agent's record
    // after every frame.
    std::vector<AgentRecord> RunCrowd(uint32 workers)
    {
        constexpr uint32 kSize = 40;
        constexpr uint32 kAgents = 48;
        constexpr uint32 kFrames = 60;
        CreateWorld(workers);
        const auto map = CreateGrid(kSize, kSize);
        BuildWall(map, kSize, kSize / 2);
        std::vector<ECS::Entity> agents;
        for (uint32 i = 0; i < kAgents; ++i)
        {
            const float32 x = 1.5f + static_cast<float32>(i % 6);
            const float32 z = 2.5f + static_cast<float32>(i / 6) * 4.0f;
            agents.push_back(CreateAgent(map, x, z, static_cast<float32>(kSize) - x, static_cast<float32>(kSize) - z));
        }

        NavigationPathfindingSystem pathSystem;
        pathSystem.MaxPathSolvesPerFrame = 8;
        NavigationMovementSystem moveSystem;
        std::vector<AgentRecord> records;
        const auto& buffer = NavigationService::Get().GetPathBuffer();
        for (uint32 frame = 0; frame < kFrames; ++frame)
        {
            pathSystem.Update(*m_World, kDt);
            m_World->ProcessCommands();
            moveSystem.Update(*m_World, kDt);
            m_World->ProcessCommands();
            for (const auto& agent : agents)
                records.push_back(Record(*agent.Get<Components::NavigationAgentState>(), buffer));
        }
        return records;
    }

    // Agents in a column on a wide grid, each heading to the far side: solves
    // long enough that pool helpers join while units are left.
    std::vector<ECS::Entity> CreateLongColumn(uint32 count)
    {
        constexpr uint32 kWidth = 160;
        const auto map = CreateGrid(kWidth, count);
        std::vector<ECS::Entity> agents;
        for (uint32 i = 0; i < count; ++i)
        {
            const float32 z = 0.5f + static_cast<float32>(i);
            agents.push_back(CreateAgent(map, 1.5f, z, static_cast<float32>(kWidth) - 1.5f,
                                         static_cast<float32>(count) - z));
        }
        return agents;
    }

    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    std::unique_ptr<ECS::World> m_World;
};

} // namespace

// The solves read grids without a lock while other threads join them, so no
// other managed system may run beside the system.
TEST(NavigationParallelSolve, SystemUpdatesExclusively)
{
    EXPECT_TRUE(NavigationPathfindingSystem{}.RequiresExclusiveUpdate());
}

TEST_F(NavigationParallelSolveTest, ResultsIdenticalWithAndWithoutPool)
{
    const std::vector<AgentRecord> serial = RunCrowd(0);
    TearDown();
    SetUp();
    const std::vector<AgentRecord> parallel = RunCrowd(kPoolWorkers);

    ASSERT_EQ(serial.size(), parallel.size());
    std::size_t solved = 0;
    for (std::size_t i = 0; i < serial.size(); ++i)
    {
        ASSERT_TRUE(serial[i] == parallel[i]) << "record " << i << " (frame " << i / 48 << ", agent " << i % 48
                                              << ") differs between no pool and a " << kPoolWorkers
                                              << "-worker pool";
        solved += serial[i].Points.empty() ? 0u : 1u;
    }
    EXPECT_GT(solved, 0u) << "the scenario must hold paths for the comparison to mean anything";
}

TEST_F(NavigationParallelSolveTest, SolvesPublishHelpersOnThePool)
{
    CreateWorld(4);
    auto agents = CreateLongColumn(8);
    NavigationPathfindingSystem pathSystem;

    const uint64 before = m_Pool->GetPublishedJobCount();
    pathSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();
    EXPECT_GT(m_Pool->GetPublishedJobCount(), before) << "eight grid solves ran without a helper on the pool";
    for (const auto& agent : agents)
        EXPECT_EQ(agent.Get<Components::NavigationAgentState>()->Status,
                  static_cast<uint8>(Pathfinding::PathStatus::Complete));
}

TEST_F(NavigationParallelSolveTest, ResultsApplyInQueueOrder)
{
    // The first queued agent has the longest route, so on a pool it finishes
    // last; its path is still the first one allocated.
    constexpr uint32 kAgents = 16;
    CreateWorld(kPoolWorkers);
    const auto map = CreateGrid(160, kAgents);
    std::vector<ECS::Entity> agents;
    for (uint32 i = 0; i < kAgents; ++i)
    {
        const float32 z = 0.5f + static_cast<float32>(i);
        agents.push_back(CreateAgent(map, 1.5f, z, 158.5f - 9.0f * static_cast<float32>(i), z));
    }
    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();

    for (uint32 i = 0; i < kAgents; ++i)
    {
        const auto* state = agents[i].Get<Components::NavigationAgentState>();
        ASSERT_EQ(state->Status, static_cast<uint8>(Pathfinding::PathStatus::Complete)) << "agent " << i;
        // A fresh PathBuffer hands out slots 0, 1, 2, ... in allocation order.
        EXPECT_EQ(state->PathIndex, i) << "agent " << i << "'s path was not the " << i << "th allocated";
    }
}

TEST_F(NavigationParallelSolveTest, NoActivePathJobsOutsideTheUpdate)
{
    CreateWorld(kPoolWorkers);
    auto agents = CreateLongColumn(16);
    NavigationPathfindingSystem pathSystem;
    EXPECT_FALSE(NavigationService::Get().HasActivePathJobs());
    pathSystem.Update(*m_World, kDt);
    EXPECT_FALSE(NavigationService::Get().HasActivePathJobs())
        << "a solve outlived its Update: map writers after the system would be deferred";
    for (const auto& agent : agents)
        EXPECT_FALSE(agent.Get<Components::NavigationAgentState>()->PathDirty);
}

TEST_F(NavigationParallelSolveTest, OneBatchSolvesOnTwoGridMaps)
{
    // One batch holds solves on both maps, so its path-solve window must cover
    // each of them: a solve's View() asserts (Debug, DebugFast) on a map
    // outside the window.
    CreateWorld(kPoolWorkers);
    auto agents = CreateLongColumn(8);
    const auto second = CreateLongColumn(8);
    agents.insert(agents.end(), second.begin(), second.end());
    const auto* first = agents.front().Get<Components::NavigationAgent>();
    const auto* last = agents.back().Get<Components::NavigationAgent>();
    ASSERT_NE(NavigationService::Get().GetGridMap({first->NavMapIndex, first->NavMapGeneration}), nullptr);
    ASSERT_NE(NavigationService::Get().GetGridMap({last->NavMapIndex, last->NavMapGeneration}), nullptr);
    ASSERT_NE(first->NavMapIndex, last->NavMapIndex);

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();
    for (const auto& agent : agents)
        EXPECT_EQ(agent.Get<Components::NavigationAgentState>()->Status,
                  static_cast<uint8>(Pathfinding::PathStatus::Complete));
}

#if GE_DEBUG_INSTRUMENTATION
namespace
{
void RecordActivePathJobs(JobSystem::WorkStealingThreadPool::ClaimHelperPoint point)
{
    // A helper counted in progress holds the caller's join, so the solves'
    // window is still open here.
    if (point != JobSystem::WorkStealingThreadPool::ClaimHelperPoint::CountedInProgress)
        return;
    auto& counter = NavigationService::Get().HasActivePathJobs() ? s_HelpersSeenWhileSolving
                                                                  : s_HelpersSeenOutsideSolving;
    counter.fetch_add(1, std::memory_order_relaxed);
}

void WriteGridFromHelper(JobSystem::WorkStealingThreadPool::ClaimHelperPoint point)
{
    if (point != JobSystem::WorkStealingThreadPool::ClaimHelperPoint::CountedInProgress)
        return;
    auto* grid = s_GridToWrite.load(std::memory_order_acquire);
    if (grid && !s_WriteFired.exchange(true))
        grid->SetCellBlocked(0, 0, true);
}

void RemoveMapFromHelper(JobSystem::WorkStealingThreadPool::ClaimHelperPoint point)
{
    if (point != JobSystem::WorkStealingThreadPool::ClaimHelperPoint::CountedInProgress)
        return;
    if (!s_WriteFired.exchange(true))
        NavigationService::Get().RemoveMap(s_MapToRemove);
}
} // namespace

TEST_F(NavigationParallelSolveTest, ActivePathJobsWhileSolvesRun)
{
    CreateWorld(kPoolWorkers);
    auto agents = CreateLongColumn(32);
    NavigationPathfindingSystem pathSystem;
    s_HelpersSeenWhileSolving = 0;
    s_HelpersSeenOutsideSolving = 0;
    JobSystem::WorkStealingThreadPool::SetClaimHelperHookForTests(&RecordActivePathJobs);
    pathSystem.Update(*m_World, kDt);
    JobSystem::WorkStealingThreadPool::SetClaimHelperHookForTests(nullptr);

    EXPECT_GT(s_HelpersSeenWhileSolving.load(), 0u) << "no pool helper ran a solve";
    EXPECT_EQ(s_HelpersSeenOutsideSolving.load(), 0u) << "a helper ran a solve with HasActivePathJobs false";
    EXPECT_FALSE(NavigationService::Get().HasActivePathJobs());
}

#if !defined(NDEBUG)
TEST_F(NavigationParallelSolveTest, GridWriteWhileSolvesRunAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            CreateWorld(kPoolWorkers);
            auto agents = CreateLongColumn(32);
            s_GridToWrite.store(NavigationService::Get().GetGridMap(
                                    {agents[0].Get<Components::NavigationAgent>()->NavMapIndex,
                                     agents[0].Get<Components::NavigationAgent>()->NavMapGeneration}),
                                std::memory_order_release);
            s_WriteFired = false;
            JobSystem::WorkStealingThreadPool::SetClaimHelperHookForTests(&WriteGridFromHelper);
            NavigationPathfindingSystem pathSystem;
            pathSystem.Update(*m_World, kDt);
        },
        "while path solves read it");
}

TEST_F(NavigationParallelSolveTest, MapTableWriteWhileSolvesRunAsserts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            CreateWorld(kPoolWorkers);
            auto agents = CreateLongColumn(32);
            s_MapToRemove.Index = agents[0].Get<Components::NavigationAgent>()->NavMapIndex;
            s_MapToRemove.Generation = agents[0].Get<Components::NavigationAgent>()->NavMapGeneration;
            s_WriteFired = false;
            JobSystem::WorkStealingThreadPool::SetClaimHelperHookForTests(&RemoveMapFromHelper);
            NavigationPathfindingSystem pathSystem;
            pathSystem.Update(*m_World, kDt);
        },
        "map table was written while path solves read it");
}
#endif
#endif
