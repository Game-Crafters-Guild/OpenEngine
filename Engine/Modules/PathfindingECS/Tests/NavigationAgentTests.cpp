#include "NavigationTestSurfaces.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationScriptApi.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"

#include "Components/Transform.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EngineLogCapture.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;
using Navigation::AgentState;
using Navigation::StatusReason;

namespace
{

constexpr float32 kDt = 1.0f / 60.0f;

class NavigationAgentTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
    }

    void TearDown() override
    {
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    // An agent standing at (x, 0, z) whose Surface is `surface` (empty: the only surface).
    ECS::Entity CreateAgent(float32 x, float32 z, ECS::EntityHandle surface = {})
    {
        auto entity = world->Create();
        Components::NavigationAgent agent{};
        agent.Surface = surface;
        agent.ReplanInterval = 0.0f;
        entity.Set(agent);
        Components::Transform transform{};
        transform.matrix[12] = x;
        transform.matrix[14] = z;
        entity.Set(transform);
        Components::WorldTransform worldTransform{};
        worldTransform.matrix[12] = x;
        worldTransform.matrix[14] = z;
        entity.Set(worldTransform);
        world->ProcessCommands();
        return entity;
    }

    // A mesh surface over level ground, `extent` meters on each side of the origin, not baked yet.
    ECS::Entity CreateMeshSurface(float32 extent)
    {
        Testing::CreateGroundPlane(*world);
        auto entity = world->Create();
        Components::NavigationSurface surface{};
        surface.FitBounds = false;
        surface.Extent = {extent, 10.0f, extent};
        entity.Set(surface);
        world->ProcessCommands();
        return entity;
    }

    void Solve(uint32 frames = 1)
    {
        for (uint32 i = 0; i < frames; ++i)
            paths.Update(*world, kDt);
    }

    // Solves and moves the agents for `frames` frames.
    void Walk(uint32 frames)
    {
        for (uint32 i = 0; i < frames; ++i)
        {
            paths.Update(*world, kDt);
            movement.Update(*world, kDt);
            world->ProcessCommands();
        }
    }

    // Orders `count` agents standing 1.2 m apart in rows of up to 10 on a baked mesh to one point,
    // walks them for 1,200 frames at 1/60 s, and counts those that read Arrived.
    int ArrivedOfAGroupOrderedToOnePoint(int count)
    {
        CreateMeshSurface(16.0f);
        Testing::BakeSurfaces(*world);
        constexpr int kRow = 10;
        std::vector<ECS::Entity> agents;
        for (int i = 0; i < count; ++i)
        {
            const int inRow = std::min(kRow, count - (i / kRow) * kRow);
            const float32 z = 1.2f * (static_cast<float32>(i % kRow) - 0.5f * static_cast<float32>(inRow - 1));
            agents.push_back(CreateAgent(-6.0f - 1.2f * static_cast<float32>(i / kRow), z));
        }
        for (auto& agent : agents)
            EXPECT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {6.0f, 0.0f, 0.0f}));
        Walk(1200);
        int arrived = 0;
        for (auto& agent : agents)
            if (Navigation::GetStatus(*world, agent.GetHandle()).State == AgentState::Arrived)
                ++arrived;
        return arrived;
    }

    const Components::NavigationAgentState& StateOf(ECS::Entity& agent)
    {
        return *agent.Get<Components::NavigationAgentState>();
    }

    std::unique_ptr<ECS::World> world;
    NavigationPathfindingSystem paths;
    NavigationMovementSystem movement;
};

} // namespace

TEST_F(NavigationAgentTest, AnEmptySurfaceWalksTheOnlySurfaceInTheScene)
{
    auto surface = Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 1.5f);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {14.5f, 0.0f, 14.5f}));
    Solve();

    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Walking);
    EXPECT_EQ(status.Reason, StatusReason::None);
    const auto* bound = agent.Get<Components::NavigationAgentState>();
    EXPECT_EQ((Pathfinding::NavMapHandle{bound->NavMapIndex, bound->NavMapGeneration}),
              Testing::SurfaceMap(*world, surface));
}

TEST_F(NavigationAgentTest, AnEmptySurfaceAmongTwoSurfacesWaitsWithNoSurface)
{
    Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto second = Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 1.5f);

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        EXPECT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {14.5f, 0.0f, 14.5f}));
        Solve(30);
        Logger::Log::Flush();
    }
    const Navigation::AgentStatus waiting = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(waiting.State, AgentState::Waiting);
    EXPECT_EQ(waiting.Reason, StatusReason::NoSurface);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "no navigation surface to walk"), 1u);

    // The order waits: naming the surface lets it go on, with no new order.
    agent.GetForWrite<Components::NavigationAgent>()->Surface = second.GetHandle();
    world->ProcessCommands();
    Solve();
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
    const auto* bound = agent.Get<Components::NavigationAgentState>();
    EXPECT_EQ((Pathfinding::NavMapHandle{bound->NavMapIndex, bound->NavMapGeneration}),
              Testing::SurfaceMap(*world, second));
}

TEST_F(NavigationAgentTest, ADestinationOffTheSurfaceIsRefusedWithItsDistance)
{
    Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 8.5f);

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        // 15 m beyond the center of the grid's last column.
        ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {30.5f, 0.0f, 8.5f}));
        Solve(30);
        Logger::Log::Flush();
    }

    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Failed);
    EXPECT_EQ(status.Reason, StatusReason::DestinationOffSurface);
    EXPECT_FALSE(StateOf(agent).HasDestination);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "is 15.0 m from the nearest walkable ground"), 1u)
        << TestLog::FirstLineContaining(lines, "Navigation agent");
}

TEST_F(NavigationAgentTest, AFailedSolveLogsItsReasonOnce)
{
    auto surface = Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto* grid = NavigationService::Get().GetGridMap(Testing::SurfaceMap(*world, surface));
    ASSERT_NE(grid, nullptr);
    for (uint32 z = 0; z < 16; ++z)
        grid->SetCellBlocked(8, z, true);
    auto agent = CreateAgent(2.5f, 8.5f);

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {13.5f, 0.0f, 8.5f}));
        // A second of frames: the Failed request is solved again every
        // NavigationPathfindingSystem::kFailedPathRetrySeconds.
        Solve(60);
        Logger::Log::Flush();
    }

    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Failed);
    EXPECT_EQ(status.Reason, StatusReason::NoRoute);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "no walkable route reaches"), 1u);
}

TEST_F(NavigationAgentTest, ARequestWaitsWhileItsSurfaceBakesAndCompletesOnceBaked)
{
    // 320 m on a side: 10 x 10 tiles of 32 m, more than one surface update bakes.
    auto surface = CreateMeshSurface(160.0f);
    auto agent = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {100.0f, 0.0f, 100.0f}));

    NavigationSurfaceSystem surfaces;
    uint32 mostPendingTiles = 0;
    for (uint32 frame = 0; frame < 30 && Navigation::GetStatus(*world, agent.GetHandle()).State != AgentState::Walking;
         ++frame)
    {
        const Navigation::AgentStatus waiting = Navigation::GetStatus(*world, agent.GetHandle());
        EXPECT_EQ(waiting.State, AgentState::Waiting);
        if (frame > 0)
            EXPECT_EQ(waiting.Reason, StatusReason::SurfaceNotBaked);
        mostPendingTiles = std::max(mostPendingTiles, waiting.PendingTiles);
        surfaces.Update(*world, kDt);
        world->ProcessCommands();
        Solve();
    }

    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Walking) << Navigation::FormatBakeStatus(*surface.Get<Components::NavigationSurface>());
    EXPECT_EQ(status.Reason, StatusReason::None);
    EXPECT_EQ(status.PendingTiles, 0u);
    EXPECT_GT(mostPendingTiles, 0u) << "the request never waited on unbaked tiles";
}

TEST_F(NavigationAgentTest, ADestinationAboveTheSurfaceMovesOntoIt)
{
    CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto agent = CreateAgent(0.0f, 0.0f);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {10.0f, 4.5f, 10.0f}));
    Solve();

    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
    EXPECT_NEAR(StateOf(agent).DestinationY, 0.0f, 0.5f);
    EXPECT_FLOAT_EQ(StateOf(agent).DestinationX, 10.0f);
}

TEST_F(NavigationAgentTest, AnAgentOnADynamicBodyWaitsWithDynamicBody)
{
    Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 1.5f);
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Dynamic;
    agent.Set(body);
    world->ProcessCommands();

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {14.5f, 0.0f, 14.5f}));
    Solve();
    Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Waiting);
    EXPECT_EQ(status.Reason, StatusReason::DynamicBody);

    agent.GetForWrite<Components::PhysicsBody>()->motionType = Physics::MotionType::Kinematic;
    Solve();
    status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Walking);
    EXPECT_EQ(status.Reason, StatusReason::None);
}

TEST_F(NavigationAgentTest, AMeshAgentArrivesAndASecondOrderWalksItAgain)
{
    CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto agent = CreateAgent(0.0f, 0.0f);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(600);
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Arrived);
    // A lone agent arrives within its Stopping Distance.
    const float32 dx = agent.Get<Components::Transform>()->matrix[12] - 6.0f;
    const float32 dz = agent.Get<Components::Transform>()->matrix[14];
    EXPECT_LE(std::sqrt(dx * dx + dz * dz), agent.Get<Components::NavigationAgent>()->StoppingDistance);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {-6.0f, 0.0f, 0.0f}));
    Walk(1);
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
    Walk(600);
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Arrived);
    EXPECT_NEAR(agent.Get<Components::Transform>()->matrix[12], -6.0f, 0.2f) << "the second order did not move the agent";
}


TEST_F(NavigationAgentTest, AMeshAgentTakesANewOrderWhileItWalks)
{
    CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto agent = CreateAgent(0.0f, 0.0f);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {10.0f, 0.0f, 0.0f}));
    Walk(30);
    ASSERT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {0.0f, 0.0f, 8.0f}));
    Walk(600);
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Arrived);
    EXPECT_NEAR(agent.Get<Components::Transform>()->matrix[14], 8.0f, 0.2f) << "the new order did not turn the agent";
}

TEST_F(NavigationAgentTest, AWalkingAgentKeepsItsPathWhileItsSurfaceBakesAgainAndReplansAfter)
{
    // 10 x 10 tiles: the rebake takes more than one surface update.
    auto surface = CreateMeshSurface(160.0f);
    NavigationSurfaceSystem surfaces;
    const auto bakeUntilBaked = [&] {
        for (int i = 0; i < 60 && surface.Get<Components::NavigationSurface>()->BakePhase !=
                                      Components::NavigationBakePhase::Baked;
             ++i)
        {
            surfaces.Update(*world, kDt);
            world->ProcessCommands();
        }
    };
    bakeUntilBaked();
    auto agent = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {20.0f, 0.0f, 0.0f}));
    Solve();
    ASSERT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
    const Pathfinding::PathHandle path{StateOf(agent).PathIndex, StateOf(agent).PathGeneration};
    ASSERT_TRUE(path.IsValid());

    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    surfaces.Update(*world, kDt);
    world->ProcessCommands();
    ASSERT_EQ(surface.Get<Components::NavigationSurface>()->BakePhase, Components::NavigationBakePhase::Baking);
    Solve();
    const Navigation::AgentStatus walking = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(walking.State, AgentState::Walking) << "a rebake held the walking agent back";
    EXPECT_EQ(walking.Reason, StatusReason::None);
    EXPECT_EQ(StateOf(agent).PathIndex, path.Index);
    EXPECT_EQ(StateOf(agent).PathGeneration, path.Generation);

    bakeUntilBaked();
    ASSERT_EQ(surface.Get<Components::NavigationSurface>()->BakePhase, Components::NavigationBakePhase::Baked);
    EXPECT_TRUE(StateOf(agent).ReplanRequested) << "the agent did not replan on the new tiles";
    Solve();
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
    EXPECT_FALSE(StateOf(agent).ReplanRequested);
}

TEST_F(NavigationAgentTest, AnOrderOnASurfaceWhoseRebakeFailedSolvesOnItsTiles)
{
    auto surface = CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto agent = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Solve();
    ASSERT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);

    // Bounds wider than a chart's 64 tiles: the rebake fails and the current tiles keep serving.
    surface.GetForWrite<Components::NavigationSurface>()->Extent = {2000.0f, 10.0f, 2000.0f};
    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    NavigationSurfaceSystem surfaces;
    surfaces.Update(*world, kDt);
    world->ProcessCommands();
    ASSERT_EQ(surface.Get<Components::NavigationSurface>()->BakePhase, Components::NavigationBakePhase::Failed);
    Solve();
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {-6.0f, 0.0f, 0.0f}));
    Solve();
    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Walking);
    EXPECT_EQ(status.Reason, StatusReason::None);
}

TEST_F(NavigationAgentTest, TheUpdatesLeaveTheAgentsConfigurationUnwritten)
{
    CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto agent = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(1);

    const ECS::ComponentTypeId configuration = ECS::GetComponentTypeId<Components::NavigationAgent>();
    const uint64 before = world->GetEntityColumnVersion(agent.GetHandle(), configuration);
    Walk(10);
    EXPECT_EQ(world->GetEntityColumnVersion(agent.GetHandle(), configuration), before);
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);
}

TEST_F(NavigationAgentTest, AWideAgentReachesADestinationBesideTheSurfaceWithinTwoOfItsRadii)
{
    Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 8.5f);
    agent.GetForWrite<Components::NavigationAgent>()->Radius = 2.0f;
    world->ProcessCommands();

    // 2 m beyond the center of the grid's last column: farther than twice the surface's 0.5 m agent
    // radius, within twice the agent's own.
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {17.5f, 0.0f, 8.5f}));
    Solve();

    EXPECT_NE(Navigation::GetStatus(*world, agent.GetHandle()).Reason, StatusReason::DestinationOffSurface);
    EXPECT_LT(StateOf(agent).DestinationX, 16.0f);
}

TEST_F(NavigationAgentTest, OnlyAMovingAgentWritesItsWorldTransform)
{
    Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 1.5f);
    const ECS::ComponentTypeId worldTransform = ECS::GetComponentTypeId<Components::WorldTransform>();
    Walk(1);

    const uint64 atRest = world->GetEntityColumnVersion(agent.GetHandle(), worldTransform);
    Walk(10);
    EXPECT_EQ(world->GetEntityColumnVersion(agent.GetHandle(), worldTransform), atRest)
        << "an agent at rest stamped its WorldTransform";

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {14.5f, 0.0f, 1.5f}));
    Walk(10);
    EXPECT_GT(world->GetEntityColumnVersion(agent.GetHandle(), worldTransform), atRest);
    EXPECT_GT(agent.Get<Components::Transform>()->matrix[12], 1.5f);
    EXPECT_EQ(agent.Get<Components::Transform>()->matrix[12], agent.Get<Components::WorldTransform>()->matrix[12]);
}

TEST_F(NavigationAgentTest, AnAgentStandingOffTheSurfaceFailsWithStartOffSurface)
{
    Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    // 24 m beyond the grid's last column.
    auto agent = CreateAgent(40.0f, 8.5f);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {8.5f, 0.0f, 8.5f}));
    Solve();

    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Failed);
    EXPECT_EQ(status.Reason, StatusReason::StartOffSurface);
}

TEST_F(NavigationAgentTest, ADestinationAnObstacleCoversFailsWithDestinationOffSurface)
{
    auto surface = Testing::CreateFlatGridSurface(*world, 16, 16, 1.0f);
    auto agent = CreateAgent(1.5f, 1.5f);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {9.5f, 0.0f, 9.5f}));
    Walk(1);
    ASSERT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Walking);

    // An obstacle covers 4 m on every side of the destination once the order is under way.
    auto* grid = NavigationService::Get().GetGridMap(Testing::SurfaceMap(*world, surface));
    ASSERT_NE(grid, nullptr);
    for (uint32 z = 5; z <= 14; ++z)
        for (uint32 x = 5; x <= 14; ++x)
            grid->SetCellBlocked(x, z, true);
    Walk(5);

    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Failed);
    EXPECT_EQ(status.Reason, StatusReason::DestinationOffSurface);
}


TEST_F(NavigationAgentTest, SixAgentsOrderedToOnePointAllArrive)
{
    EXPECT_EQ(ArrivedOfAGroupOrderedToOnePoint(6), 6);
}

TEST_F(NavigationAgentTest, TwelveAgentsOrderedToOnePointAllArrive)
{
    EXPECT_EQ(ArrivedOfAGroupOrderedToOnePoint(12), 12);
}

TEST_F(NavigationAgentTest, FortyAgentsOrderedToOnePointAllArrive)
{
    EXPECT_EQ(ArrivedOfAGroupOrderedToOnePoint(40), 40);
}

TEST_F(NavigationAgentTest, AnArrivedMeshAgentStaysInTheCrowdUntilClearDestination)
{
    CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto agent = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(600);
    ASSERT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Arrived);
    EXPECT_GE(StateOf(agent).CrowdAgentHandle, 0) << "an arrived agent left the crowd";

    Navigation::ClearDestination(*world, agent.GetHandle());
    Walk(1);
    EXPECT_EQ(Navigation::GetStatus(*world, agent.GetHandle()).State, AgentState::Idle);
    EXPECT_LT(StateOf(agent).CrowdAgentHandle, 0);
}

TEST_F(NavigationAgentTest, AnArrivedMeshAgentYieldsToAPush)
{
    CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto first = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, first.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(600);
    ASSERT_EQ(Navigation::GetStatus(*world, first.GetHandle()).State, AgentState::Arrived);
    const float32 restX = first.Get<Components::Transform>()->matrix[12];
    const float32 restZ = first.Get<Components::Transform>()->matrix[14];

    // A late agent ordered to the same point, which steps in 0.6 m from the arrived one (their
    // radii sum to 1 m), pushes it aside.
    auto late = CreateAgent(restX - 0.6f, restZ);
    ASSERT_TRUE(Navigation::SetDestination(*world, late.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(120);
    const float32 dx = first.Get<Components::Transform>()->matrix[12] - restX;
    const float32 dz = first.Get<Components::Transform>()->matrix[14] - restZ;
    EXPECT_GT(std::sqrt(dx * dx + dz * dz), 0.05f) << "the arrived agent did not yield to the push";
    EXPECT_EQ(Navigation::GetStatus(*world, first.GetHandle()).State, AgentState::Arrived);
    EXPECT_EQ(Navigation::GetStatus(*world, late.GetHandle()).State, AgentState::Arrived);
}

TEST_F(NavigationAgentTest, AStalledAgentThatIsNotHeldOffByOthersKeepsItsOrder)
{
    // A 12 m wall across x = 0 on level ground.
    Testing::CreateGroundPlane(*world);
    auto wall = world->Create();
    Components::PhysicsBody body{};
    body.motionType = Physics::MotionType::Static;
    wall.Set(body);
    wall.Set(Components::PhysicsCollider{});
    Components::BoxColliderShape box{};
    box.halfExtentsX = 0.25f;
    box.halfExtentsY = 2.0f;
    box.halfExtentsZ = 6.0f;
    wall.Set(box);
    Components::Transform wallTransform{};
    wallTransform.matrix[13] = 2.0f;
    wall.Set(wallTransform);
    Components::WorldTransform wallWorld{};
    wallWorld.matrix[13] = 2.0f;
    wall.Set(wallWorld);
    world->ProcessCommands();
    auto surface = world->Create();
    Components::NavigationSurface navigation{};
    navigation.FitBounds = false;
    navigation.Extent = {16.0f, 10.0f, 16.0f};
    surface.Set(navigation);
    world->ProcessCommands();
    Testing::BakeSurfaces(*world);

    // Speed 0: one 3 m from its goal with the wall between, one 3 m from its goal on open ground.
    auto behindTheWall = CreateAgent(-1.5f, 0.0f);
    auto inTheOpen = CreateAgent(-1.5f, 10.0f);
    behindTheWall.GetForWrite<Components::NavigationAgent>()->Speed = 0.0f;
    inTheOpen.GetForWrite<Components::NavigationAgent>()->Speed = 0.0f;
    world->ProcessCommands();
    ASSERT_TRUE(Navigation::SetDestination(*world, behindTheWall.GetHandle(), {1.5f, 0.0f, 0.0f}));
    ASSERT_TRUE(Navigation::SetDestination(*world, inTheOpen.GetHandle(), {-1.5f, 0.0f, 13.0f}));
    Walk(60);

    EXPECT_EQ(Navigation::GetStatus(*world, behindTheWall.GetHandle()).State, AgentState::Walking);
    EXPECT_EQ(Navigation::GetStatus(*world, inTheOpen.GetHandle()).State, AgentState::Walking);
    EXPECT_TRUE(StateOf(behindTheWall).HasDestination);
    EXPECT_TRUE(StateOf(inTheOpen).HasDestination);
}

TEST_F(NavigationAgentTest, AnOrderWhileTheSurfaceBakesANewLayoutWaits)
{
    // 10 x 10 tiles, baked; then bounds of 13 x 13 tiles: the rebake starts from an empty mesh.
    auto surface = CreateMeshSurface(160.0f);
    NavigationSurfaceSystem surfaces;
    for (int i = 0; i < 80 && surface.Get<Components::NavigationSurface>()->BakePhase !=
                                  Components::NavigationBakePhase::Baked;
         ++i)
    {
        surfaces.Update(*world, kDt);
        world->ProcessCommands();
    }
    ASSERT_EQ(surface.Get<Components::NavigationSurface>()->BakePhase, Components::NavigationBakePhase::Baked);
    auto agent = CreateAgent(0.0f, 0.0f);
    surface.GetForWrite<Components::NavigationSurface>()->Extent = {200.0f, 10.0f, 200.0f};
    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    world->ProcessCommands();
    surfaces.Update(*world, kDt);
    world->ProcessCommands();
    ASSERT_EQ(surface.Get<Components::NavigationSurface>()->BakePhase, Components::NavigationBakePhase::Baking);

    ASSERT_TRUE(Navigation::SetDestination(*world, agent.GetHandle(), {20.0f, 0.0f, 0.0f}));
    Solve();
    const Navigation::AgentStatus status = Navigation::GetStatus(*world, agent.GetHandle());
    EXPECT_EQ(status.State, AgentState::Waiting);
    EXPECT_EQ(status.Reason, StatusReason::SurfaceNotBaked);
}

TEST_F(NavigationAgentTest, AnArrivedMeshAgentKeepsItsPlaceWhenTheCrowdStartsAgain)
{
    auto surface = CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto first = CreateAgent(0.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, first.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(600);
    ASSERT_EQ(Navigation::GetStatus(*world, first.GetHandle()).State, AgentState::Arrived);

    // A new cell size is a new layout: the rebake starts the crowd again, empty.
    surface.GetForWrite<Components::NavigationSurface>()->CellSize = 0.25f;
    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    world->ProcessCommands();
    for (int i = 0; i < 64; ++i)
        Testing::BakeSurfaces(*world);
    auto second = CreateAgent(-6.0f, -6.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, second.GetHandle(), {-6.0f, 0.0f, 6.0f}));
    Walk(120);

    EXPECT_NEAR(first.Get<Components::Transform>()->matrix[12], 6.0f, 0.5f) << "the arrived agent left its place";
    EXPECT_NEAR(first.Get<Components::Transform>()->matrix[14], 0.0f, 0.5f) << "the arrived agent left its place";
    EXPECT_EQ(Navigation::GetStatus(*world, first.GetHandle()).State, AgentState::Arrived);
    EXPECT_NE(StateOf(first).CrowdAgentHandle, StateOf(second).CrowdAgentHandle);
}

TEST_F(NavigationAgentTest, AnAgentArrivesAgainstAnArrivedAgentAfterTheCrowdStartsAgain)
{
    auto surface = CreateMeshSurface(16.0f);
    Testing::BakeSurfaces(*world);
    auto first = CreateAgent(-6.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, first.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(1200);
    ASSERT_EQ(Navigation::GetStatus(*world, first.GetHandle()).State, AgentState::Arrived);

    // A new cell size is a new layout: the rebake starts the crowd again, and the arrived agent
    // rejoins it as a stationary member that has arrived at its goal.
    surface.GetForWrite<Components::NavigationSurface>()->CellSize = 0.25f;
    ASSERT_TRUE(Navigation::RequestBake(*world, surface.GetHandle()));
    world->ProcessCommands();
    for (int i = 0; i < 64; ++i)
        Testing::BakeSurfaces(*world);

    // A second agent ordered to the same point is held off by the first and arrives against it.
    auto second = CreateAgent(-6.0f, 0.0f);
    ASSERT_TRUE(Navigation::SetDestination(*world, second.GetHandle(), {6.0f, 0.0f, 0.0f}));
    Walk(900);
    EXPECT_EQ(Navigation::GetStatus(*world, second.GetHandle()).State, AgentState::Arrived);
    EXPECT_EQ(Navigation::GetStatus(*world, first.GetHandle()).State, AgentState::Arrived);
}
