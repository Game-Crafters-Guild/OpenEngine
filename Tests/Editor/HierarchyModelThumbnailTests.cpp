// The Hierarchy's model-thumbnail lookup (FindModelGuidForEntityOrChildren) with and
// without the caller's child lists: the row binds pass the panel's tree, so the walk
// must find what the whole-world scan finds, and follow the lists it is given.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"
#include "Types/FlatMap.h"

#include <span>
#include <vector>

namespace
{
using GameEngine::FlatMap;
using GameEngine::GUID;
using GameEngine::uint32;
using GameEngine::Components::MeshRenderer;
using GameEngine::Components::Parent;
using GameEngine::Components::Transform;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::FindModelGuidForEntityOrChildren;

// Each parent's children, in list order, as the Hierarchy panel's provider keeps them.
class ChildLists
{
public:
    void Add(EntityHandle parent, EntityHandle child) { m_Children.GetOrInsert(parent.id).push_back(child); }

    std::span<const EntityHandle> operator()(EntityHandle parent) const
    {
        const std::vector<EntityHandle>* children = m_Children.Find(parent.id);
        if (!children)
            return {};
        return *children;
    }

private:
    FlatMap<uint32, std::vector<EntityHandle>> m_Children;
};

EntityHandle MakeNode(World& world, ChildLists& lists, EntityHandle parent = {})
{
    const EntityHandle entity = world.CreateEntity();
    world.AddComponentImmediate<Transform>(entity, Transform{});
    if (parent.IsValid())
    {
        world.AddComponentImmediate<Parent>(entity, Parent{parent});
        lists.Add(parent, entity);
    }
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
    EntityHandle Root;
    EntityHandle Pivot;
    EntityHandle Part;
};

Unit MakeUnit(World& world, ChildLists& lists, const GUID& model)
{
    Unit unit;
    unit.Root = MakeNode(world, lists);
    unit.Pivot = MakeNode(world, lists, unit.Root);
    unit.Part = MakeNode(world, lists, unit.Pivot);
    DrawModel(world, unit.Part, model);
    return unit;
}

TEST(HierarchyModelThumbnail, TheChildListsFindTheModelTheScanFinds)
{
    World world;
    ChildLists lists;
    const GUID model = GUID::Generate();
    const Unit unit = MakeUnit(world, lists, model);
    MakeUnit(world, lists, GUID::Generate()); // a neighbor the walk must not reach

    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.Root), model);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.Root, lists), model);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.Pivot, lists), model);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.Part, lists), model);
}

TEST(HierarchyModelThumbnail, TheWalkFollowsTheListsItIsGiven)
{
    World world;
    ChildLists before;
    const GUID model = GUID::Generate();
    const Unit unit = MakeUnit(world, before, model);
    const EntityHandle elsewhere = MakeNode(world, before);

    // The world moves the pivot. The walk answers from the lists, which hold the
    // tree the rows show, until the lists move the pivot too.
    world.GetComponentForWrite<Parent>(unit.Pivot)->Entity = elsewhere;
    EXPECT_TRUE(FindModelGuidForEntityOrChildren(&world, unit.Root).IsNull());
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, unit.Root, before), model);

    ChildLists after;
    after.Add(elsewhere, unit.Pivot);
    after.Add(unit.Pivot, unit.Part);
    EXPECT_TRUE(FindModelGuidForEntityOrChildren(&world, unit.Root, after).IsNull());
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, elsewhere, after), model);
}

TEST(HierarchyModelThumbnail, TheWalkTakesTheShallowestModelInListOrder)
{
    World world;
    ChildLists lists;
    const EntityHandle root = MakeNode(world, lists);
    const EntityHandle branch = MakeNode(world, lists, root);
    const GUID deep = GUID::Generate();
    DrawModel(world, MakeNode(world, lists, branch), deep);
    const GUID first = GUID::Generate();
    DrawModel(world, MakeNode(world, lists, root), first);
    const GUID second = GUID::Generate();
    DrawModel(world, MakeNode(world, lists, root), second);

    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, root, lists), first);
    EXPECT_EQ(FindModelGuidForEntityOrChildren(&world, branch, lists), deep);
}

} // namespace
