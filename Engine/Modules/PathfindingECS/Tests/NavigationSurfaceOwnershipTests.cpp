#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationScriptApi.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "NavigationTestSurfaces.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{
using Surface = Components::NavigationSurface;
using Map = Pathfinding::NavMapHandle;
Map Handle(const Surface& surface) { return {surface.NavMapIndex, surface.NavMapGeneration}; }
void ExpectBindingEqual(const Surface& actual, const Surface& expected)
{
    EXPECT_EQ(Handle(actual), Handle(expected));
    EXPECT_EQ(actual.OwnerWorldId, expected.OwnerWorldId);
}

// An 8 x 8 grid surface over level ground, cell (0, 0) at the origin.
Surface MakeGridSurface()
{
    Surface surface{};
    surface.Mode = Components::NavigationSurfaceMode::Grid;
    surface.CellSize = 1.0f;
    surface.FitBounds = false;
    surface.Center = {4.0f, 0.0f, 4.0f};
    surface.Extent = {4.0f, 1.0f, 4.0f};
    return surface;
}

class NavigationSurfaceOwnershipTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
        Testing::CreateGroundPlane(*world);
    }
    void TearDown() override
    {
        world.reset();
        NavigationService::Shutdown();
    }
    ECS::EntityHandle Make(ECS::World& owner)
    {
        const auto entity = owner.Create(MakeGridSurface()).GetHandle();
        build.Update(owner, 0);
        return entity;
    }
    // Rebuilds the service and adds one map into the slot the original held.
    // The slot repeats; the generation cannot, so the old handle names nothing.
    Map RecreateService(Map original)
    {
        auto* previous = NavigationService::Get().GetGridMap(original);
        if (!previous) return {};
        const auto settings = previous->GetSettings();
        NavigationService::Shutdown();
        NavigationService::Initialize();
        const auto unrelated = NavigationService::Get().AddGridMap(settings);
        EXPECT_EQ(original.Index, unrelated.Index);
        EXPECT_NE(original.Generation, unrelated.Generation);
        NavigationService::Get().GetGridMap(unrelated)->SetCellDynamicBlocked(2, 3, true);
        return unrelated;
    }
    void ExpectSentinel(Map map)
    {
        auto* sentinel = NavigationService::Get().GetGridMap(map);
        ASSERT_NE(sentinel, nullptr) << "foreign map was retired through a stale binding";
        EXPECT_TRUE(sentinel->IsCellBlocked(2, 3)) << "foreign map was mutated through a stale binding";
    }
    std::unique_ptr<ECS::World> world;
    NavigationSurfaceSystem build;
};

TEST_F(NavigationSurfaceOwnershipTest, RecreatedServiceIsRejectedBeforeBakeAndDynamicWrites)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Surface>(entity)));
    ASSERT_TRUE(unrelated.IsValid());
    build.Update(*world, 0);
    const auto* rebuilt = world->GetComponent<Surface>(entity);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_NE(Handle(*rebuilt), unrelated);
    EXPECT_TRUE(ResolveNavigationSurfaceMap(*world, *rebuilt).IsValid());
    ExpectSentinel(unrelated);
}

// Play-mode exit and WorldSnapshotCommand undo restore the world from raw component bytes: Clear
// releases every map, then the restored surface still names the released handle. The next update
// must bake it into a new map.
TEST_F(NavigationSurfaceOwnershipTest, WorldSnapshotRestoreRebakesTheGrid)
{
    const auto entity = Make(*world);
    const auto before = Handle(*world->GetComponent<Surface>(entity));
    const auto snapshot = world->SerializeWorld();
    world->DeserializeWorld(snapshot);
    const auto* restored = world->GetComponent<Surface>(entity);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(Handle(*restored), before);
    EXPECT_EQ(NavigationService::Get().GetGridMap(before), nullptr);
    EXPECT_EQ(ResolveNavigationSurfaceGrid(*world, *restored), nullptr);
    build.Update(*world, 0);
    restored = world->GetComponent<Surface>(entity);
    EXPECT_NE(Handle(*restored), before);
    EXPECT_NE(ResolveNavigationSurfaceGrid(*world, *restored), nullptr);
    EXPECT_EQ(restored->BakePhase, Components::NavigationBakePhase::Baked);
}

TEST_F(NavigationSurfaceOwnershipTest, WorldSnapshotRestoreRebakesTheMesh)
{
    Surface mesh{};
    mesh.FitBounds = false;
    mesh.Extent = {8.0f, 1.0f, 8.0f};
    const auto entity = world->Create(mesh).GetHandle();
    build.Update(*world, 0);
    const Map before = Handle(*world->GetComponent<Surface>(entity));
    ASSERT_NE(NavigationService::Get().GetDetourNavMap(before), nullptr);
    const auto snapshot = world->SerializeWorld();
    world->DeserializeWorld(snapshot);
    EXPECT_EQ(NavigationService::Get().GetDetourNavMap(before), nullptr);
    build.Update(*world, 0);
    const Map after = Handle(*world->GetComponent<Surface>(entity));
    EXPECT_NE(after, before);
    EXPECT_NE(NavigationService::Get().GetDetourNavMap(after), nullptr);
}

enum class Removal { Destroy, Clear, SchemaRemove };
class NavigationSurfaceRemovalTest : public NavigationSurfaceOwnershipTest,
                                     public ::testing::WithParamInterface<Removal>
{
protected:
    void Remove(ECS::EntityHandle entity)
    {
        const auto* schema = Scene::SceneSchemaRegistry::Find("NavigationSurface");
        ASSERT_NE(schema, nullptr);
        switch (GetParam())
        {
        case Removal::Destroy: world->DestroyEntityImmediate(entity); break;
        case Removal::Clear: world->Clear(); break;
        case Removal::SchemaRemove: ASSERT_TRUE(schema->Remove(*world, entity)); break;
        }
    }
};

TEST_P(NavigationSurfaceRemovalTest, StaleServiceReleasePreservesUnrelatedMap)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Surface>(entity)));
    ASSERT_TRUE(unrelated.IsValid());
    Remove(entity);
    ExpectSentinel(unrelated);
}

TEST_P(NavigationSurfaceRemovalTest, CurrentOwnerReleasesAndSlotIsReusable)
{
    const auto entity = Make(*world);
    const auto owned = Handle(*world->GetComponent<Surface>(entity));
    const auto settings = NavigationService::Get().GetGridMap(owned)->GetSettings();
    Remove(entity);
    EXPECT_EQ(NavigationService::Get().GetGridMap(owned), nullptr);
    const auto replacement = NavigationService::Get().AddGridMap(settings);
    EXPECT_EQ(replacement.Index, owned.Index);
    EXPECT_NE(replacement.Generation, owned.Generation);
    // Schema removal plus the registered hook must not retire the slot twice.
    const auto second = NavigationService::Get().AddGridMap(settings);
    EXPECT_NE(second.Index, replacement.Index);
}
INSTANTIATE_TEST_SUITE_P(AllRemovalOwners, NavigationSurfaceRemovalTest,
    ::testing::Values(Removal::Destroy, Removal::Clear, Removal::SchemaRemove));

TEST_F(NavigationSurfaceOwnershipTest, ReusedBuilderRejectsCopiedBindingFromAnotherWorld)
{
    const auto first = Make(*world);
    const Surface copied = *world->GetComponent<Surface>(first);
    const auto original = Handle(copied);
    NavigationService::Get().GetGridMap(original)->SetCellDynamicBlocked(2, 3, true);
    ECS::World other;
    const auto second = other.Create(copied).GetHandle();
    build.Update(other, 0);
    EXPECT_NE(Handle(*other.GetComponent<Surface>(second)), original);
    ExpectSentinel(original);
    // `other` registers no hooks, so its map is handed back by hand.
    NavigationService::Get().RemoveMap(Handle(*other.GetComponent<Surface>(second)));
}

TEST_F(NavigationSurfaceOwnershipTest, ForgottenCopyInTheSameWorldRebindsWithoutReleasingTheOriginal)
{
    const auto first = Make(*world);
    const auto original = Handle(*world->GetComponent<Surface>(first));
    // Static blocking: the update legitimately rewrites the original's dynamic-blocked cells while
    // it stays resolvable.
    NavigationService::Get().GetGridMap(original)->SetCellBlocked(2, 3, true);
    const auto copy = world->CloneEntity(first);
    auto* copied = world->GetComponentForWrite<Surface>(copy);
    ASSERT_NE(copied, nullptr);
    EXPECT_EQ(ResolveNavigationSurfaceGrid(*world, *copied), NavigationService::Get().GetGridMap(original));
    ForgetNavigationSurfaceMap(*copied);
    EXPECT_EQ(copied->OwnerWorldId, 0u);
    ExpectSentinel(original);
    build.Update(*world, 0);
    const auto rebound = Handle(*world->GetComponent<Surface>(copy));
    EXPECT_NE(rebound, original);
    EXPECT_NE(NavigationService::Get().GetGridMap(rebound), nullptr);
    world->DestroyEntityImmediate(copy);
    ExpectSentinel(original);
    EXPECT_EQ(NavigationService::Get().GetGridMap(rebound), nullptr);
}

TEST_F(NavigationSurfaceOwnershipTest, ClearRejectsSavedRuntimeCopyBeforeBuilderReuse)
{
    const auto first = Make(*world);
    const Surface copied = *world->GetComponent<Surface>(first);
    const auto old = Handle(copied);
    world->Clear();
    Testing::CreateGroundPlane(*world);
    const auto second = world->Create(copied).GetHandle();
    build.Update(*world, 0);
    EXPECT_NE(Handle(*world->GetComponent<Surface>(second)), old);
    EXPECT_NE(NavigationService::Get().GetGridMap(Handle(*world->GetComponent<Surface>(second))), nullptr);
}

TEST_F(NavigationSurfaceOwnershipTest, ActualDiskLoadOmitsRuntimeBindingAndRebakes)
{
    const auto entity = Make(*world);
    const auto old = Handle(*world->GetComponent<Surface>(entity));
    const auto path = std::filesystem::temp_directory_path() /
        ("navigation-surface-owner-" + std::to_string(world->GetWorldId()) + ".scene");
    struct FileCleanup { std::filesystem::path Path; ~FileCleanup() { std::error_code error; std::filesystem::remove(Path, error); } } cleanup{path};
    ASSERT_TRUE(Scene::SaveSceneToFile(*world, path));
    std::ifstream input(path);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(text.find("NavMap"), std::string::npos);
    EXPECT_EQ(text.find("OwnerWorld"), std::string::npos);
    EXPECT_EQ(text.find("Bake"), std::string::npos);
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options; options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(*world, path, options));
    ASSERT_FALSE(degradation.IsDegraded());
    EXPECT_EQ(NavigationService::Get().GetGridMap(old), nullptr);
    EXPECT_EQ(world->Query<ECS::Read<Surface>>().Count(), 1u);
    world->Query<ECS::Read<Surface>>().Each([](const Surface& surface) {
        EXPECT_EQ(surface.NavMapGeneration, 0u);
        EXPECT_EQ(surface.OwnerWorldId, 0u);
    });
    build.Update(*world, 0);
    world->Query<ECS::Read<Surface>>().Each([](const Surface& surface) {
        EXPECT_EQ(surface.BakePhase, Components::NavigationBakePhase::Baked);
        EXPECT_NE(NavigationService::Get().GetGridMap(Handle(surface)), nullptr);
    });
}

TEST_F(NavigationSurfaceOwnershipTest, ManualCreationPublishesCurrentOwner)
{
    Surface surface = MakeGridSurface();
    const auto handle = CreateNavigationSurfaceMap(*world, surface);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(surface.OwnerWorldId, world->GetWorldId());
    EXPECT_NE(ResolveNavigationSurfaceGrid(*world, surface), nullptr);
    EXPECT_EQ(ResolveNavigationSurfaceMesh(*world, surface), nullptr);
    ResetNavigationSurfaceMap(*world, surface);
    EXPECT_EQ(NavigationService::Get().GetGridMap(handle), nullptr);
    EXPECT_EQ(surface.OwnerWorldId, 0u);
    ResetNavigationSurfaceMap(*world, surface);
}

TEST_F(NavigationSurfaceOwnershipTest, CreateRefusesCurrentOwnerWithoutReplacingOrMutatingIt)
{
    const auto entity = Make(*world);
    auto* surface = world->GetComponentForWrite<Surface>(entity);
    const Surface original = *surface;
    auto* map = ResolveNavigationSurfaceGrid(*world, *surface);
    ASSERT_NE(map, nullptr);
    map->SetCellDynamicBlocked(2, 3, true);
    EXPECT_FALSE(CreateNavigationSurfaceMap(*world, *surface).IsValid());
    ExpectBindingEqual(*surface, original);
    ExpectSentinel(Handle(original));
    EXPECT_EQ(map->GetSettings().Width, 8u);
}

TEST_F(NavigationSurfaceOwnershipTest, StaleCreationNeverReleasesOrAdoptsTheRecreatedServicesMap)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Surface>(entity)));
    auto* surface = world->GetComponentForWrite<Surface>(entity);
    const auto original = *surface;
    EXPECT_FALSE(ResolveNavigationSurfaceMap(*world, *surface).IsValid());
    ExpectBindingEqual(*surface, original); // Resolving is read-only.
    const auto replacement = CreateNavigationSurfaceMap(*world, *surface);
    EXPECT_TRUE(replacement.IsValid());
    EXPECT_NE(replacement, unrelated);
    EXPECT_NE(ResolveNavigationSurfaceGrid(*world, *surface), nullptr);
    ExpectSentinel(unrelated);
}

TEST_F(NavigationSurfaceOwnershipTest, MissingServiceCreationLeavesExistingComponentUnchanged)
{
    const auto entity = Make(*world);
    auto* surface = world->GetComponentForWrite<Surface>(entity);
    const Surface original = *surface;
    NavigationService::Shutdown();
    EXPECT_FALSE(CreateNavigationSurfaceMap(*world, *surface).IsValid());
    ExpectBindingEqual(*surface, original);
    EXPECT_FALSE(ResolveNavigationSurfaceMap(*world, *surface).IsValid());
    ResetNavigationSurfaceMap(*world, *surface);
    EXPECT_EQ(surface->OwnerWorldId, 0u);
}

TEST_F(NavigationSurfaceOwnershipTest, UnstampedBindingIsNeverAdoptedOrReleased)
{
    Pathfinding::GridSettings settings{}; settings.Width = settings.Depth = 8;
    const auto unrelated = NavigationService::Get().AddGridMap(settings);
    NavigationService::Get().GetGridMap(unrelated)->SetCellDynamicBlocked(2, 3, true);
    Surface unstamped{};
    unstamped.NavMapIndex = unrelated.Index; unstamped.NavMapGeneration = unrelated.Generation;
    EXPECT_FALSE(ResolveNavigationSurfaceMap(*world, unstamped).IsValid());
    ResetNavigationSurfaceMap(*world, unstamped);
    EXPECT_EQ(unstamped.NavMapGeneration, 0u);
    ExpectSentinel(unrelated);
}

// A copy of a bound surface in another world names the original world's map. Removing the copy,
// through the scene schema or by clearing that world, leaves the original's map alone.
TEST_F(NavigationSurfaceOwnershipTest, ForeignCopyRemovedThroughTheSchemaKeepsTheOriginalsMap)
{
    const auto entity = Make(*world);
    const auto original = *world->GetComponent<Surface>(entity);
    NavigationService::Get().GetGridMap(Handle(original))->SetCellDynamicBlocked(2, 3, true);
    ECS::World other;
    RegisterNavigationWorldHooks(other);
    const auto copy = other.Create(original).GetHandle();
    const auto* schema = Scene::SceneSchemaRegistry::Find("NavigationSurface");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->Remove(other, copy));
    ExpectSentinel(Handle(original));
    other.Create(original);
    other.Clear();
    ExpectSentinel(Handle(original));
}

// Undo and redo restore a surface's whole component, binding included. A Mode switch baked between
// them makes a map of the other kind; whichever map the restored bytes stop naming is released.
TEST_F(NavigationSurfaceOwnershipTest, UndoAndRedoAcrossAModeSwitchLeaveNoOrphanedMap)
{
    Surface mesh{};
    mesh.FitBounds = false;
    mesh.Extent = {8.0f, 1.0f, 8.0f};
    const auto entity = world->Create(mesh).GetHandle();
    build.Update(*world, 0);
    const Surface beforeSwitch = *world->GetComponent<Surface>(entity);
    ASSERT_NE(NavigationService::Get().GetDetourNavMap(Handle(beforeSwitch)), nullptr);

    // The inspector commits the switch, then the user presses Bake.
    Surface switched = beforeSwitch;
    switched.Mode = Components::NavigationSurfaceMode::Grid;
    switched.CellSize = 1.0f;
    world->AddComponentImmediate(entity, switched);
    const Surface afterSwitch = *world->GetComponent<Surface>(entity);
    ASSERT_TRUE(Navigation::RequestBake(*world, entity));
    build.Update(*world, 0);
    const Map grid = Handle(*world->GetComponent<Surface>(entity));
    ASSERT_NE(NavigationService::Get().GetGridMap(grid), nullptr);

    world->AddComponentImmediate(entity, beforeSwitch); // undo
    build.Update(*world, 0);
    const Map meshAgain = Handle(*world->GetComponent<Surface>(entity));
    EXPECT_NE(NavigationService::Get().GetDetourNavMap(meshAgain), nullptr);
    EXPECT_EQ(NavigationService::Get().GetMap(grid), nullptr) << "undo left the switch's grid behind";

    world->AddComponentImmediate(entity, afterSwitch); // redo
    build.Update(*world, 0);
    EXPECT_NE(NavigationService::Get().GetGridMap(Handle(*world->GetComponent<Surface>(entity))), nullptr);
    EXPECT_EQ(NavigationService::Get().GetMap(meshAgain), nullptr) << "redo left the undo's mesh behind";
}

// Undoing an edit made after a re-bake restores the bytes of the older bake; the status the map
// holds is the newer one.
TEST_F(NavigationSurfaceOwnershipTest, UndoKeepsTheStatusOfTheMapTheSurfaceHolds)
{
    const auto entity = Make(*world);
    const Surface beforeEdit = *world->GetComponent<Surface>(entity);
    Surface edited = beforeEdit;
    edited.AgentRadius = 0.7f;
    world->AddComponentImmediate(entity, edited);
    ASSERT_TRUE(Navigation::RequestBake(*world, entity));
    build.Update(*world, 0);
    const uint64 newerVersion = world->GetComponent<Surface>(entity)->BakeVersion;
    ASSERT_GT(newerVersion, beforeEdit.BakeVersion);

    world->AddComponentImmediate(entity, beforeEdit); // undo the radius edit
    build.Update(*world, 0);
    const auto* restored = world->GetComponent<Surface>(entity);
    EXPECT_EQ(restored->BakeVersion, newerVersion);
    EXPECT_TRUE(restored->SettingsChanged) << "the map was baked with the radius the undo took away";
}

TEST_F(NavigationSurfaceOwnershipTest, MovementBeforeRebakeDoesNotExpireForeignReservations)
{
    const auto entity = Make(*world);
    const auto unrelated = RecreateService(Handle(*world->GetComponent<Surface>(entity)));
    auto* sentinel = NavigationService::Get().GetGridMap(unrelated);
    ASSERT_NE(sentinel, nullptr);
    sentinel->ReserveCell(4, 5, 117, .5f);
    NavigationMovementSystem movement;
    movement.Update(*world, 1);
    EXPECT_EQ(sentinel->GetCellReservation(4, 5).AgentId, 117u);
    ExpectSentinel(unrelated);
}

TEST_F(NavigationSurfaceOwnershipTest, MovementStillExpiresCurrentOwnerReservations)
{
    const auto entity = Make(*world);
    auto* map = ResolveNavigationSurfaceGrid(*world, *world->GetComponent<Surface>(entity));
    ASSERT_NE(map, nullptr);
    map->ReserveCell(4, 5, 117, .5f);
    NavigationMovementSystem movement;
    movement.Update(*world, 1);
    EXPECT_EQ(map->GetCellReservation(4, 5).AgentId, 0u);
}
} // namespace
