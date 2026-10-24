// The derived half of the Scene View eye: SceneViewHiddenInHierarchy, derived from
// SceneViewHidden and the Parent chain by the same pass the enable state uses.

#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/SceneViewHidden.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/SceneViewHiddenInHierarchySystem.h"
#include "ECS/World.h"

using namespace GameEngine;
using GameEngine::Components::Parent;
using GameEngine::Components::SceneViewHidden;
using GameEngine::Components::SceneViewHiddenInHierarchy;
using GameEngine::Components::Transform;

namespace
{
ECS::EntityHandle MakeNode(ECS::World& world, ECS::EntityHandle parent = {})
{
    ECS::Entity entity = world.Create();
    entity.Set(Transform{});
    if (parent.IsValid())
        entity.Set(Parent{parent});
    world.ProcessCommands();
    return entity.GetHandle();
}

bool HiddenInHierarchy(ECS::World& world, ECS::EntityHandle entity)
{
    return world.HasComponent<SceneViewHiddenInHierarchy>(entity);
}
} // namespace

// A child follows its parent's eye and keeps its own: showing the parent shows the child again
// unless the child's own eye is closed. The eye leaves the enable state alone.
TEST(SceneViewHiddenInHierarchyTests, AChildFollowsItsParentsEyeAndKeepsItsOwn)
{
    ECS::World world(nullptr);
    ECS::SceneViewHiddenInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    const ECS::EntityHandle child = MakeNode(world, root);
    const ECS::EntityHandle grandchild = MakeNode(world, child);
    const ECS::EntityHandle sibling = MakeNode(world);
    system.Update(world, 0.0f);

    world.AddComponentImmediate<SceneViewHidden>(root, {});
    system.Update(world, 0.0f);
    EXPECT_TRUE(HiddenInHierarchy(world, root));
    EXPECT_TRUE(HiddenInHierarchy(world, child));
    EXPECT_TRUE(HiddenInHierarchy(world, grandchild));
    EXPECT_FALSE(HiddenInHierarchy(world, sibling));
    EXPECT_TRUE(ECS::Entity(&world, child).IsEnabledInHierarchy());

    world.AddComponentImmediate<SceneViewHidden>(grandchild, {});
    world.RemoveComponentImmediate<SceneViewHidden>(root);
    system.Update(world, 0.0f);
    EXPECT_FALSE(HiddenInHierarchy(world, root));
    EXPECT_FALSE(HiddenInHierarchy(world, child));
    EXPECT_TRUE(HiddenInHierarchy(world, grandchild));

    // Reparenting under a hidden entity hides the moved subtree.
    world.AddComponentImmediate<SceneViewHidden>(sibling, {});
    system.Update(world, 0.0f);
    world.GetComponentForWrite<Parent>(child)->Entity = sibling;
    system.Update(world, 0.0f);
    EXPECT_TRUE(HiddenInHierarchy(world, child));
    EXPECT_FALSE(HiddenInHierarchy(world, root));
}
