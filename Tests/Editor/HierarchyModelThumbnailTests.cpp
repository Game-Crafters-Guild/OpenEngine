// The Hierarchy's model-thumbnail lookup (FindModelGuidForEntityOrChildren) with and
// without a ParentRelationIndex: the row binds pass the panel's index, so they must
// find what the whole-world scan finds, after the hierarchy changes too.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"

namespace
{
using GameEngine::GUID;
using GameEngine::Components::MeshRenderer;
using GameEngine::Components::Parent;
using GameEngine::Components::ParentRelationIndex;
using GameEngine::Components::Transform;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::FindModelGuidForEntityOrChildren;

EntityHandle MakeNode(World& world, EntityHandle parent = {})
{
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate<Transform>(entity, Transform{});
    if (parent.IsValid())
        world.AddComponentImmediate<Parent>(entity, Parent{parent});
    return entity;
}

void DrawModel(World& world, EntityHandle entity, const GUID& model)
{
    MeshRenderer renderer{};
    renderer.modelAssetGuid.Set(model);
    world.AddComponentImmediate<MeshRenderer>(entity, renderer);
}

// A unit as a blueprint instance makes one: a root that draws nothing, a
// pivot, and the part that draws the model two levels down.
struct Unit
{
    EntityHandle root, pivot, part;
};

Unit MakeUnit(World& world, const GUID& model)
{
    Unit u;
    u.root = MakeNode(world);
    u.pivot = MakeNode(world, u.root);
    u.part = MakeNode(world, u.pivot);
    DrawModel(world, u.part, model);
    return u;
}

TEST(HierarchyModelThumbnail, TheIndexFindsTheModelTheScanFinds)
{
    World world;
    const GUID model = GUID::Generate();
    const Unit unit = MakeUnit(world, model);
    MakeUnit(world, GUID::Generate());  // a neighbour the walk must not reach
    ParentRelationIndex index;

    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.root), model);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.root, &index), model);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.pivot, &index), model);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.part, &index), model);
}

TEST(HierarchyModelThumbnail, TheIndexFollowsAStructuralChange)
{
    World world;
    const Unit unit = MakeUnit(world, GUID::Generate());
    ParentRelationIndex index;
    ASSERT_FALSE(FindModelGuidForEntityOrChildren(&world, unit.root, &index).IsNull());

    // Destroying the part bumps the structural version, so the index rebuilds itself.
    world.DestroyEntityImmediate(unit.part);
    EXPECT_TRUE(FindModelGuidForEntityOrChildren(&world, unit.root).IsNull());
    EXPECT_TRUE(FindModelGuidForEntityOrChildren(&world, unit.root, &index).IsNull());

    // So does a new part.
    const GUID other = GUID::Generate();
    DrawModel(world, MakeNode(world, unit.pivot), other);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.root, &index), other);
}

TEST(HierarchyModelThumbnail, AReparentByValueWriteNeedsTheIndexDropped)
{
    World world;
    const GUID model = GUID::Generate();
    const Unit unit = MakeUnit(world, model);
    const EntityHandle elsewhere = MakeNode(world);
    ParentRelationIndex index;
    ASSERT_EQ(FindModelGuidForEntityOrChildren(&world, unit.root, &index), model);

    // A Parent value write moves the pivot without a structural change: the
    // index cannot see it, which is why the panel drops its index whenever it
    // applies such a move (and on every full refresh).
    world.GetComponentForWrite<Parent>(unit.pivot)->parent = elsewhere;
    EXPECT_TRUE(FindModelGuidForEntityOrChildren(&world, unit.root).IsNull());
    index.Invalidate();
    EXPECT_TRUE(FindModelGuidForEntityOrChildren(&world, unit.root, &index).IsNull());
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, elsewhere, &index), model);
}

} // namespace
