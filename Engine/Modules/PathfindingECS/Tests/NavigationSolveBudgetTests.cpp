// NavigationPathfindingSystem's solve budget: at most MaxPathSolvesPerFrame
// solves per Update, oldest request first, whatever raised the request, with
// the rest queued.
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "NavigationTestSurfaces.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "Components/Transform.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/PathfindingTypes.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

constexpr float32 kDt = 1.0f / 60.0f;

Pathfinding::PathStatus StatusOf(const Components::NavigationAgentState& state)
{
    return static_cast<Pathfinding::PathStatus>(state.Status);
}

bool HasPath(const Components::NavigationAgentState& state)
{
    const auto status = StatusOf(state);
    return !state.PathDirty &&
           (status == Pathfinding::PathStatus::Complete || status == Pathfinding::PathStatus::Partial);
}

class NavigationSolveBudgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        m_World = std::make_unique<ECS::World>();
    }

    void TearDown() override
    {
        m_World.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    Pathfinding::NavMapHandle CreateGrid(uint32 width, uint32 depth)
    {
        auto entity = Testing::CreateFlatGridSurface(*m_World, width, depth, 1.0f);
        return Testing::SurfaceMap(*m_World, entity);
    }

    ECS::Entity CreateAgent(Pathfinding::NavMapHandle map, float32 startX, float32 startZ, float32 destX,
                            float32 destZ, float32 replanInterval = 0.0f)
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
        agent.ReplanInterval = replanInterval;
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

    // Agents in a column at x = 1.5, one per row, each heading to the far
    // side of a width x depth grid.
    std::vector<ECS::Entity> CreateColumn(Pathfinding::NavMapHandle map, uint32 count, uint32 width,
                                          float32 replanInterval = 0.0f)
    {
        std::vector<ECS::Entity> agents;
        for (uint32 i = 0; i < count; ++i)
        {
            const float32 z = 0.5f + static_cast<float32>(i);
            agents.push_back(CreateAgent(map, 1.5f, z, static_cast<float32>(width) - 1.5f, z, replanInterval));
        }
        return agents;
    }

    // A 20 m x 20 m mesh surface baked over level ground centred on the origin, with the crowd
    // NavigationSurfaceSystem initializes on the first baked mesh. An invalid handle when the
    // Detour backend is compiled out.
    Pathfinding::NavMapHandle CreateCrowdPlane()
    {
        Testing::CreateGroundPlane(*m_World, 10.0f);
        auto entity = m_World->Create();
        Components::NavigationSurface surface{};
        surface.FitBounds = false;
        surface.Center = {0.0f, 0.0f, 0.0f};
        surface.Extent = {10.0f, 1.0f, 10.0f};
        entity.Set(surface);
        m_World->ProcessCommands();
        Testing::BakeSurfaces(*m_World);
        if (!NavigationService::Get().GetCrowdManager().IsInitialized())
            return {};
        return Testing::SurfaceMap(*m_World, entity);
    }

    void Step(NavigationPathfindingSystem& pathSystem)
    {
        pathSystem.Update(*m_World, kDt);
        m_World->ProcessCommands();
    }

    void Redirty(ECS::Entity& entity)
    {
        auto* state = entity.GetForWrite<Components::NavigationAgentState>();
        ASSERT_NE(state, nullptr);
        state->PathDirty = true;
        m_World->ProcessCommands();
    }

    static uint32 CountWithPath(const std::vector<ECS::Entity>& agents)
    {
        uint32 count = 0;
        for (const auto& e : agents)
            count += HasPath(*e.Get<Components::NavigationAgentState>()) ? 1u : 0u;
        return count;
    }

    static uint32 CountDirty(const std::vector<ECS::Entity>& agents)
    {
        uint32 count = 0;
        for (const auto& e : agents)
            count += e.Get<Components::NavigationAgentState>()->PathDirty ? 1u : 0u;
        return count;
    }

    std::unique_ptr<ECS::World> m_World;
};

} // namespace

TEST_F(NavigationSolveBudgetTest, ServesAtMostBudgetOldestFirst)
{
    constexpr uint32 kBudget = 4;
    const auto map = CreateGrid(24, 24);
    auto agents = CreateColumn(map, 3 * kBudget, 24);

    NavigationPathfindingSystem pathSystem;
    pathSystem.MaxPathSolvesPerFrame = kBudget;
    Step(pathSystem);

    EXPECT_EQ(CountWithPath(agents), kBudget);
    std::vector<ECS::Entity> solvedFirst;
    std::vector<ECS::Entity> waiting;
    for (auto& e : agents)
    {
        const auto* state = e.Get<Components::NavigationAgentState>();
        if (HasPath(*state))
        {
            EXPECT_EQ(state->PathRequestSequence, 0u);
            solvedFirst.push_back(e);
            continue;
        }
        EXPECT_TRUE(state->PathDirty);
        EXPECT_EQ(StatusOf(*state), Pathfinding::PathStatus::Pending);
        EXPECT_NE(state->PathRequestSequence, 0u);
        waiting.push_back(e);
    }
    ASSERT_EQ(solvedFirst.size(), kBudget);
    ASSERT_EQ(waiting.size(), 2 * kBudget);

    const auto& stats = pathSystem.GetStatistics();
    EXPECT_EQ(stats.NewRequests, 3 * kBudget);
    EXPECT_EQ(stats.Solved, kBudget);
    EXPECT_EQ(stats.Backlog, 2 * kBudget);
    EXPECT_EQ(stats.OldestWaitFrames, 0u);

    // The agents solved first ask again. They come first in query order, so a
    // budget that served query order (or the newest requests) would solve
    // them again; oldest first serves the ones that have been waiting.
    for (auto& e : solvedFirst)
        Redirty(e);
    Step(pathSystem);

    for (const auto& e : solvedFirst)
        EXPECT_TRUE(e.Get<Components::NavigationAgentState>()->PathDirty);
    EXPECT_EQ(CountWithPath(waiting), kBudget);
    EXPECT_EQ(pathSystem.GetStatistics().Solved, kBudget);
    EXPECT_EQ(pathSystem.GetStatistics().LongestSolvedWaitFrames, 1u);
    EXPECT_EQ(pathSystem.GetStatistics().OldestWaitFrames, 1u);
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 2 * kBudget);
}

TEST_F(NavigationSolveBudgetTest, EngineTriggersShareTheBudget)
{
    constexpr uint32 kBudget = 2;
    constexpr uint32 kAgents = 8;
    constexpr uint32 kWidth = 24;
    const auto map = CreateGrid(kWidth, kAgents);
    auto* grid = NavigationService::Get().GetGridMap(map);
    ASSERT_NE(grid, nullptr);

    // Corridor check: every agent is walking east when a wall closes the
    // grid ahead of them, and the movement system re-dirties all of them in
    // one frame.
    {
        auto walkers = CreateColumn(map, kAgents, kWidth);
        NavigationPathfindingSystem pathSystem;
        pathSystem.MaxPathSolvesPerFrame = kAgents;
        Step(pathSystem);
        ASSERT_EQ(CountWithPath(walkers), kAgents);

        NavigationMovementSystem moveSystem;
        for (int i = 0; i < 5; ++i)
        {
            moveSystem.Update(*m_World, kDt);
            m_World->ProcessCommands();
        }
        for (uint32 z = 0; z < kAgents; ++z)
            grid->SetCellBlocked(6, z, true);
        moveSystem.Update(*m_World, kDt);
        m_World->ProcessCommands();
        ASSERT_EQ(CountDirty(walkers), kAgents) << "the wall must re-dirty every walker";

        pathSystem.MaxPathSolvesPerFrame = kBudget;
        Step(pathSystem);
        EXPECT_EQ(CountDirty(walkers), kAgents - kBudget);
        EXPECT_EQ(pathSystem.GetStatistics().Solved, kBudget);

        for (auto& e : walkers)
            m_World->DestroyEntityImmediate(e.GetHandle());
        for (uint32 z = 0; z < kAgents; ++z)
            grid->SetCellBlocked(6, z, false);
    }

    // Failed retry: every destination is blocked, so every agent fails, and
    // all of them retry in the same frame kFailedPathRetrySeconds later.
    {
        for (uint32 z = 0; z < kAgents; ++z)
            grid->SetCellBlocked(kWidth - 2, z, true);
        auto stuck = CreateColumn(map, kAgents, kWidth);
        NavigationPathfindingSystem pathSystem;
        pathSystem.MaxPathSolvesPerFrame = kAgents;
        Step(pathSystem);
        for (const auto& e : stuck)
            ASSERT_EQ(StatusOf(*e.Get<Components::NavigationAgentState>()), Pathfinding::PathStatus::Failed);

        // The retry re-arms the timer to kFailedPathRetrySeconds in the
        // Update it fires, solved or not.
        pathSystem.MaxPathSolvesPerFrame = kBudget;
        bool retried = false;
        for (int frame = 0; frame < 60 && !retried; ++frame)
        {
            Step(pathSystem);
            for (const auto& e : stuck)
                retried = retried || e.Get<Components::NavigationAgentState>()->ReplanTimer ==
                                         NavigationPathfindingSystem::kFailedPathRetrySeconds;
        }
        ASSERT_TRUE(retried) << "the Failed retry never fired";
        EXPECT_EQ(CountDirty(stuck), kAgents - kBudget);
        EXPECT_EQ(pathSystem.GetStatistics().Solved, kBudget);
    }
}

TEST_F(NavigationSolveBudgetTest, EveryRequestServedWithinBacklogOverBudgetFrames)
{
    constexpr uint32 kBudget = 3;
    constexpr uint32 kAgents = 10;
    constexpr uint32 kBound = (kAgents + kBudget - 1) / kBudget;
    const auto map = CreateGrid(16, kAgents);
    auto agents = CreateColumn(map, kAgents, 16);

    NavigationPathfindingSystem pathSystem;
    pathSystem.MaxPathSolvesPerFrame = kBudget;
    std::vector<int> lastSolved(kAgents, -1);
    for (int frame = 0; frame < 40; ++frame)
    {
        for (auto& e : agents)
            Redirty(e);
        Step(pathSystem);
        uint32 solvedThisFrame = 0;
        for (uint32 i = 0; i < kAgents; ++i)
        {
            if (agents[i].Get<Components::NavigationAgentState>()->PathDirty)
            {
                EXPECT_LT(frame - lastSolved[i], static_cast<int>(kBound) + 1)
                    << "agent " << i << " has waited past ceil(backlog / budget) frames";
                continue;
            }
            lastSolved[i] = frame;
            ++solvedThisFrame;
        }
        EXPECT_EQ(solvedThisFrame, kBudget);
    }
    EXPECT_LE(pathSystem.GetStatistics().OldestWaitFrames, kBound);
}

TEST_F(NavigationSolveBudgetTest, StampDiesWithEntityAndWorldClear)
{
    const auto map = CreateGrid(16, 16);
    auto& pathBuffer = NavigationService::Get().GetPathBuffer();
    NavigationPathfindingSystem pathSystem;

    auto first = CreateAgent(map, 1.5f, 1.5f, 14.5f, 1.5f);
    auto second = CreateAgent(map, 1.5f, 3.5f, 14.5f, 3.5f);
    Step(pathSystem);
    const auto* secondState = second.Get<Components::NavigationAgentState>();
    ASSERT_TRUE(HasPath(*secondState));
    const Pathfinding::PathHandle oldPath{secondState->PathIndex, secondState->PathGeneration};

    // Both ask again with room for one; query order solves the first. The
    // queued one gives up its old path and waits as Pending.
    pathSystem.MaxPathSolvesPerFrame = 1;
    Redirty(first);
    Redirty(second);
    Step(pathSystem);
    ASSERT_TRUE(HasPath(*first.Get<Components::NavigationAgentState>()));
    ECS::Entity queued = second;
    const auto* queuedState = queued.Get<Components::NavigationAgentState>();
    ASSERT_NE(queuedState->PathRequestSequence, 0u);
    EXPECT_EQ(StatusOf(*queuedState), Pathfinding::PathStatus::Pending);
    EXPECT_EQ(queuedState->PathIndex, 0u);
    EXPECT_EQ(pathBuffer.GetPointCount(oldPath), 0u) << "the queued agent's old path must be freed";
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 1u);

    // Destroying the queued agent empties the queue.
    m_World->DestroyEntityImmediate(queued.GetHandle());
    Step(pathSystem);
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 0u);
    EXPECT_EQ(pathSystem.GetStatistics().Solved, 0u);
    EXPECT_EQ(pathSystem.GetStatistics().OldestWaitFrames, 0u);

    // A world clear with a request queued: the next agent is served as a new
    // request.
    auto a = CreateAgent(map, 1.5f, 5.5f, 14.5f, 5.5f);
    auto b = CreateAgent(map, 1.5f, 7.5f, 14.5f, 7.5f);
    Step(pathSystem);
    ASSERT_EQ(pathSystem.GetStatistics().Backlog, 1u);
    m_World->Clear();
    m_World->ProcessCommands();
    Step(pathSystem);
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 0u);

    const auto clearedMap = CreateGrid(16, 16);
    auto reused = CreateAgent(clearedMap, 1.5f, 1.5f, 14.5f, 1.5f);
    Step(pathSystem);
    EXPECT_TRUE(HasPath(*reused.Get<Components::NavigationAgentState>()));
    EXPECT_EQ(pathSystem.GetStatistics().NewRequests, 1u);
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 0u);
    EXPECT_EQ(pathSystem.GetStatistics().LongestSolvedWaitFrames, 0u);
}

TEST_F(NavigationSolveBudgetTest, QueuedAgentReportsNoVelocity)
{
    const auto map = CreateGrid(16, 16);
    auto first = CreateAgent(map, 1.5f, 1.5f, 14.5f, 1.5f);
    auto second = CreateAgent(map, 1.5f, 3.5f, 14.5f, 3.5f);
    NavigationPathfindingSystem pathSystem;
    Step(pathSystem);

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 5; ++i)
    {
        moveSystem.Update(*m_World, kDt);
        m_World->ProcessCommands();
    }
    ASSERT_GT(second.Get<Components::NavigationAgentState>()->VelocityX, 0.0f) << "the agent must be walking east";

    // Both ask again with room for one; query order solves the first.
    pathSystem.MaxPathSolvesPerFrame = 1;
    Redirty(first);
    Redirty(second);
    Step(pathSystem);
    ASSERT_TRUE(HasPath(*first.Get<Components::NavigationAgentState>()));
    ASSERT_EQ(StatusOf(*second.Get<Components::NavigationAgentState>()), Pathfinding::PathStatus::Pending);

    const float32 queuedX = second.Get<Components::WorldTransform>()->matrix[12];
    moveSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();
    const auto* queuedState = second.Get<Components::NavigationAgentState>();
    EXPECT_EQ(queuedState->VelocityX, 0.0f);
    EXPECT_EQ(queuedState->VelocityY, 0.0f);
    EXPECT_EQ(queuedState->VelocityZ, 0.0f);
    EXPECT_EQ(second.Get<Components::WorldTransform>()->matrix[12], queuedX);
}

TEST_F(NavigationSolveBudgetTest, WithdrawnRequestRejoinsBehindNewerRequests)
{
    const auto map = CreateGrid(16, 16);
    auto agents = CreateColumn(map, 4, 16);
    NavigationPathfindingSystem pathSystem;
    pathSystem.MaxPathSolvesPerFrame = 1;
    Step(pathSystem);
    ASSERT_TRUE(HasPath(*agents[0].Get<Components::NavigationAgentState>()));

    // The second agent's request is withdrawn for one Update while the third
    // is solved, then issued again: it now joins behind the fourth.
    agents[1].GetForWrite<Components::NavigationAgentState>()->PathDirty = false;
    m_World->ProcessCommands();
    Step(pathSystem);
    ASSERT_TRUE(HasPath(*agents[2].Get<Components::NavigationAgentState>()));
    EXPECT_EQ(agents[1].Get<Components::NavigationAgentState>()->PathRequestSequence, 0u);

    Redirty(agents[1]);
    Step(pathSystem);
    EXPECT_TRUE(HasPath(*agents[3].Get<Components::NavigationAgentState>()));
    const auto* rejoined = agents[1].Get<Components::NavigationAgentState>();
    EXPECT_TRUE(rejoined->PathDirty);
    EXPECT_EQ(StatusOf(*rejoined), Pathfinding::PathStatus::Pending);
    EXPECT_EQ(pathSystem.GetStatistics().NewRequests, 1u);
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 1u);
}

TEST_F(NavigationSolveBudgetTest, TimerRefreshKeepsWalkingUntilSolved)
{
    constexpr float32 kLongInterval = 100.0f;
    const auto map = CreateGrid(16, 16);
    auto agents = CreateColumn(map, 2, 16, kLongInterval);
    auto& pathBuffer = NavigationService::Get().GetPathBuffer();
    NavigationPathfindingSystem pathSystem;
    Step(pathSystem);
    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 5; ++i)
    {
        moveSystem.Update(*m_World, kDt);
        m_World->ProcessCommands();
    }

    // Both timers expire with room for one solve; query order solves the first.
    for (auto& e : agents)
        e.GetForWrite<Components::NavigationAgentState>()->ReplanTimer = 0.0f;
    m_World->ProcessCommands();
    pathSystem.MaxPathSolvesPerFrame = 1;
    Step(pathSystem);
    auto& queued = agents[1];
    const auto* queuedState = queued.Get<Components::NavigationAgentState>();
    ASSERT_NE(queuedState->PathRequestSequence, 0u);
    EXPECT_TRUE(queuedState->ReplanRequested);
    EXPECT_EQ(StatusOf(*queuedState), Pathfinding::PathStatus::Complete);
    const Pathfinding::PathHandle oldPath{queuedState->PathIndex, queuedState->PathGeneration};
    ASSERT_GT(pathBuffer.GetPointCount(oldPath), 0u) << "a timer refresh keeps its path while it waits";

    const float32 queuedX = queued.Get<Components::WorldTransform>()->matrix[12];
    moveSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();
    EXPECT_GT(queued.Get<Components::WorldTransform>()->matrix[12], queuedX);
    EXPECT_GT(queued.Get<Components::NavigationAgentState>()->VelocityX, 0.0f);

    // The next Update solves it: the new path replaces the old one.
    Step(pathSystem);
    queuedState = queued.Get<Components::NavigationAgentState>();
    EXPECT_FALSE(queuedState->ReplanRequested);
    EXPECT_EQ(queuedState->PathRequestSequence, 0u);
    EXPECT_TRUE(HasPath(*queuedState));
    EXPECT_EQ(pathBuffer.GetPointCount(oldPath), 0u) << "the old path must be freed when the new one lands";
}

TEST_F(NavigationSolveBudgetTest, CorridorCheckDropsAQueuedTimerRefresh)
{
    constexpr float32 kLongInterval = 100.0f;
    constexpr uint32 kAgents = 3;
    const auto map = CreateGrid(16, kAgents);
    auto* grid = NavigationService::Get().GetGridMap(map);
    ASSERT_NE(grid, nullptr);
    auto agents = CreateColumn(map, kAgents, 16, kLongInterval);
    auto& pathBuffer = NavigationService::Get().GetPathBuffer();
    NavigationPathfindingSystem pathSystem;
    Step(pathSystem);
    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 5; ++i)
    {
        moveSystem.Update(*m_World, kDt);
        m_World->ProcessCommands();
    }

    // Every timer expires with room for one solve: the last two wait,
    // walking their old paths.
    for (auto& e : agents)
        e.GetForWrite<Components::NavigationAgentState>()->ReplanTimer = 0.0f;
    m_World->ProcessCommands();
    pathSystem.MaxPathSolvesPerFrame = 1;
    Step(pathSystem);
    auto& last = agents[kAgents - 1];
    const uint32 place = last.Get<Components::NavigationAgentState>()->PathRequestSequence;
    ASSERT_NE(place, 0u);
    const Pathfinding::PathHandle oldPath{last.Get<Components::NavigationAgentState>()->PathIndex,
                                          last.Get<Components::NavigationAgentState>()->PathGeneration};
    ASSERT_GT(pathBuffer.GetPointCount(oldPath), 0u);

    // A wall closes ahead of every walker, and the corridor check re-dirties
    // them while the last one is still queued: it drops its path and waits
    // as Pending, in the same place.
    for (uint32 z = 0; z < kAgents; ++z)
        grid->SetCellBlocked(6, z, true);
    moveSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();
    ASSERT_TRUE(last.Get<Components::NavigationAgentState>()->PathDirty);
    Step(pathSystem);
    const auto* lastState = last.Get<Components::NavigationAgentState>();
    EXPECT_EQ(lastState->PathRequestSequence, place);
    EXPECT_EQ(StatusOf(*lastState), Pathfinding::PathStatus::Pending);
    EXPECT_EQ(lastState->PathIndex, 0u);
    EXPECT_EQ(pathBuffer.GetPointCount(oldPath), 0u) << "the dropped path must be freed";
}

TEST_F(NavigationSolveBudgetTest, ZeroBudgetSolvesOne)
{
    const auto map = CreateGrid(16, 16);
    auto agents = CreateColumn(map, 2, 16);
    NavigationPathfindingSystem pathSystem;
    pathSystem.MaxPathSolvesPerFrame = 0;
    Step(pathSystem);
    EXPECT_EQ(CountWithPath(agents), 1u);
    EXPECT_EQ(pathSystem.GetStatistics().Backlog, 1u);
}

TEST_F(NavigationSolveBudgetTest, CrowdAgentDeferredByTheBudgetIsSolvedLater)
{
#if !GE_PATHFINDING_BACKEND_DETOUR
    GTEST_SKIP() << "the Detour backend is compiled out: no navmesh and no crowd";
#endif
    const auto map = CreateCrowdPlane();
    ASSERT_TRUE(map.IsValid()) << "the navmesh plane or its crowd failed to build";
    std::vector<ECS::Entity> agents{CreateAgent(map, -5.0f, -2.0f, 5.0f, -2.0f),
                                    CreateAgent(map, -5.0f, 2.0f, 5.0f, 2.0f)};
    NavigationPathfindingSystem pathSystem;
    pathSystem.MaxPathSolvesPerFrame = 1;
    NavigationMovementSystem moveSystem;

    // The budget defers the second agent; the movement system then hands
    // both to the crowd.
    Step(pathSystem);
    ASSERT_EQ(CountWithPath(agents), 1u);
    ASSERT_NE(agents[1].Get<Components::NavigationAgentState>()->PathRequestSequence, 0u);
    moveSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();
    ASSERT_GE(agents[1].Get<Components::NavigationAgentState>()->CrowdAgentHandle, 0);
    // A second movement Update reaches the retarget branch, which must keep the request.
    moveSystem.Update(*m_World, kDt);
    m_World->ProcessCommands();

    Step(pathSystem);
    const auto* deferred = agents[1].Get<Components::NavigationAgentState>();
    EXPECT_TRUE(HasPath(*deferred)) << "the deferred request must still be solved";
    EXPECT_NE(StatusOf(*deferred), Pathfinding::PathStatus::Pending);
    EXPECT_EQ(deferred->PathRequestSequence, 0u);
}

TEST_F(NavigationSolveBudgetTest, TimerRefreshOnAnInvalidMapWaitsAsAPendingRequest)
{
    constexpr float32 kLongInterval = 100.0f;
    const auto map = CreateGrid(16, 16);
    auto agent = CreateAgent(map, 1.5f, 0.5f, 14.5f, 0.5f, kLongInterval);
    auto& pathBuffer = NavigationService::Get().GetPathBuffer();
    NavigationPathfindingSystem pathSystem;
    Step(pathSystem);
    const auto* state = agent.Get<Components::NavigationAgentState>();
    ASSERT_TRUE(HasPath(*state));
    const Pathfinding::PathHandle oldPath{state->PathIndex, state->PathGeneration};

    // The timer fires the Update the agent's map handle goes invalid.
    agent.GetForWrite<Components::NavigationAgentState>()->ReplanTimer = 0.0f;
    agent.GetForWrite<Components::NavigationAgent>()->NavMapGeneration = 0;
    m_World->ProcessCommands();
    Step(pathSystem);
    state = agent.Get<Components::NavigationAgentState>();
    EXPECT_FALSE(state->ReplanRequested);
    EXPECT_TRUE(state->PathDirty);
    EXPECT_EQ(StatusOf(*state), Pathfinding::PathStatus::Pending);
    EXPECT_EQ(state->PathIndex, 0u);
    EXPECT_EQ(pathBuffer.GetPointCount(oldPath), 0u);

    // The request is solved once the agent has a map again.
    agent.GetForWrite<Components::NavigationAgent>()->NavMapGeneration = map.Generation;
    m_World->ProcessCommands();
    Step(pathSystem);
    EXPECT_TRUE(HasPath(*agent.Get<Components::NavigationAgentState>()));
}
