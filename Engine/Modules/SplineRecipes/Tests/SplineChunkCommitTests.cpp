// The chunk commit: how a spline recipe's generated meshes are registered under
// stable keys, drawn, and released (SplineRecipes/SplineChunkCommit.h). Driven on a
// registry with no device — registration then keeps its keys and handles
// without uploading, which is all the lifecycle under test reads — and a bare
// world.

#include "SplineRecipes/SplineChunkCommit.h"

#include "Assets/ModelAsset.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Engine/Rendering/MeshGPURegistry.h"

#include <gtest/gtest.h>

#include <vector>

using namespace GameEngine;
using SplineRecipes::SplineChunkCommit;
using Rendering::MeshGPUHandle;
using Rendering::MeshGPUKey;
using Rendering::MeshGPURegistry;

namespace
{

Mesh Triangle(float lift)
{
    Mesh mesh;
    Vertex a{};
    Vertex b{};
    Vertex c{};
    a.Position[1] = lift;
    b.Position[0] = 1.0f;
    b.Position[1] = lift;
    c.Position[2] = 1.0f;
    c.Position[1] = lift;
    mesh.Vertices = {a, b, c};
    mesh.Indices = {0u, 1u, 2u};
    mesh.MaxBounds[0] = 1.0f;
    mesh.MaxBounds[2] = 1.0f;
    mesh.MinBounds[1] = mesh.MaxBounds[1] = lift;
    return mesh;
}

MeshGPUKey KeyOf(const char* name, uint32 index = 0u)
{
    return MeshGPUKey{GUID::Derive(GUID::Null(), name), index};
}

ECS::EntityHandle FirstChild(ECS::World& world, ECS::EntityHandle owner)
{
    ECS::EntityHandle child{};
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&](ECS::EntityHandle entity, const Components::Parent& parent)
        {
            if (parent.Entity == owner)
                child = entity;
        });
    return child;
}

size_t ChildCount(ECS::World& world, ECS::EntityHandle owner)
{
    size_t count = 0;
    world.Query<ECS::Read<Components::Parent>>().Each(
        [&](ECS::EntityHandle, const Components::Parent& parent)
        {
            if (parent.Entity == owner)
                ++count;
        });
    return count;
}

} // namespace

// A key registered twice is updated in place under the same handle, a key a
// build keeps survives it with that handle, and a key a build reaches no more
// is released.
TEST(SplineChunkCommit, KeysSurviveTheBuildsThatReachThemAndNoOther)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    const MeshGPUHandle first = commit.Register(registry, KeyOf("a"), Triangle(0.0f));
    commit.Register(registry, KeyOf("b"), Triangle(1.0f));
    commit.RetireUnreached(registry);
    ASSERT_NE(registry.FindByKey(KeyOf("a")), nullptr);
    ASSERT_NE(registry.FindByKey(KeyOf("b")), nullptr);

    EXPECT_EQ(commit.Register(registry, KeyOf("a"), Triangle(2.0f)), first)
        << "a re-registered key must keep its handle";
    commit.RetireUnreached(registry);
    EXPECT_NE(registry.FindByKey(KeyOf("a")), nullptr);
    EXPECT_EQ(registry.FindByKey(KeyOf("b")), nullptr) << "an unreached key must be released";

    MeshGPUHandle kept;
    EXPECT_TRUE(commit.Keep(KeyOf("a"), kept));
    EXPECT_EQ(kept, first);
    EXPECT_FALSE(commit.Keep(KeyOf("b"), kept)) << "a released key is not kept";
    commit.RetireUnreached(registry);
    EXPECT_NE(registry.FindByKey(KeyOf("a")), nullptr) << "a kept key must survive";
}

// Slots are addressed by index: an empty mesh leaves a hole that is not a dead
// chunk, a shorter build retires the tail and releases its keys, and a
// steady-state rebuild spawns nothing.
TEST(SplineChunkCommit, SlotsKeepTheirKeysLeaveHolesAndRetireTheTail)
{
    ECS::World world;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    const ECS::EntityHandle owner = world.Create().GetHandle();
    const GUID space = GUID::Derive(GUID::Null(), "space");
    const Components::MeshRenderer renderer{};

    const std::vector<Mesh> three = {Triangle(0.0f), Mesh{}, Triangle(2.0f)};
    EXPECT_TRUE(commit.CommitSlots(world, registry, owner, space, three, renderer, true));
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 2u);
    EXPECT_NE(registry.FindByKey({space, 0u}), nullptr);
    EXPECT_EQ(registry.FindByKey({space, 1u}), nullptr) << "a hole registers nothing";
    EXPECT_NE(registry.FindByKey({space, 2u}), nullptr);
    EXPECT_TRUE(commit.ChunksAlive(world)) << "a hole is not a dead chunk";

    EXPECT_FALSE(commit.CommitSlots(world, registry, owner, space, three, renderer, true))
        << "a steady-state rebuild spawns nothing";

    const std::vector<Mesh> one = {Triangle(0.0f)};
    EXPECT_TRUE(commit.CommitSlots(world, registry, owner, space, one, renderer, true));
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 1u);
    EXPECT_EQ(registry.FindByKey({space, 2u}), nullptr) << "the retired tail keeps its key";

    EXPECT_TRUE(commit.Retire(world, &registry));
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 0u);
    EXPECT_EQ(registry.FindByKey({space, 0u}), nullptr);
}

// After a world reset the chunk handles alias whatever recycled their
// indices: they are forgotten, never destroyed, and the keys still go.
TEST(SplineChunkCommit, AWorldResetForgetsTheChunksAndReleasesTheKeys)
{
    ECS::World world;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    const ECS::EntityHandle owner = world.Create().GetHandle();
    const GUID space = GUID::Derive(GUID::Null(), "space");
    const std::vector<Mesh> meshes = {Triangle(0.0f)};
    commit.CommitSlots(world, registry, owner, space, meshes, Components::MeshRenderer{}, true);
    world.ProcessCommands();
    ASSERT_EQ(ChildCount(world, owner), 1u);

    commit.DropAfterWorldReset(&registry);
    world.ProcessCommands();
    EXPECT_EQ(ChildCount(world, owner), 1u) << "nothing may be destroyed after a reset";
    EXPECT_EQ(registry.FindByKey({space, 0u}), nullptr);
}

// The transform hierarchy runs before the recipes in a frame, and extraction skips a renderable whose
// WorldTransform is unresolved. A new chunk starts at its owner's world pose, so it draws in the frame
// that updates its neighbours in place (a road's trim and the junction lid that fills it).
TEST(SplineChunkCommit, ANewChunkStartsAtItsOwnersWorldPose)
{
    ECS::World world;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    Components::WorldTransform ownerWorld{};
    ownerWorld.Position = Mathematics::WorldPosition{5.0, 2.0, -3.0};
    ownerWorld.Version = 7u;
    ECS::Entity ownerEntity = world.Create();
    ownerEntity.Set(Components::Transform{});
    ownerEntity.Set(ownerWorld);
    world.ProcessCommands();
    const ECS::EntityHandle owner = ownerEntity.GetHandle();
    const std::vector<Mesh> meshes = {Triangle(0.0f)};

    commit.CommitSlots(world, registry, owner, GUID::Derive(GUID::Null(), "space"), meshes,
                       Components::MeshRenderer{}, true);
    world.ProcessCommands();

    const ECS::EntityHandle chunk = FirstChild(world, owner);
    ASSERT_TRUE(world.IsValid(chunk));
    const auto* chunkWorld = world.GetComponent<Components::WorldTransform>(chunk);
    ASSERT_NE(chunkWorld, nullptr);
    EXPECT_NE(chunkWorld->Version, 0u) << "an unresolved chunk is skipped by extraction for one frame";
    EXPECT_TRUE(chunkWorld->Position == ownerWorld.Position);
    for (int column = 0; column < 3; ++column)
        EXPECT_TRUE(chunkWorld->Basis[column] == ownerWorld.Basis[column]) << "basis column " << column;
}

// An owner the hierarchy has not resolved yet gives no pose: the chunk waits for the hierarchy.
TEST(SplineChunkCommit, ANewChunkUnderAnUnresolvedOwnerWaitsForTheHierarchy)
{
    ECS::World world;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    ECS::Entity ownerEntity = world.Create();
    ownerEntity.Set(Components::Transform{});
    world.ProcessCommands();
    const ECS::EntityHandle owner = ownerEntity.GetHandle();
    const std::vector<Mesh> meshes = {Triangle(0.0f)};

    commit.CommitSlots(world, registry, owner, GUID::Derive(GUID::Null(), "space"), meshes,
                       Components::MeshRenderer{}, true);
    world.ProcessCommands();

    const auto* chunkWorld = world.GetComponent<Components::WorldTransform>(FirstChild(world, owner));
    ASSERT_NE(chunkWorld, nullptr);
    EXPECT_EQ(chunkWorld->Version, 0u);
}
