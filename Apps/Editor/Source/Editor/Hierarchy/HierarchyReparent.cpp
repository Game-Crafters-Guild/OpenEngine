#include "Editor/Hierarchy/HierarchyReparent.h"

#include "Editor/Hierarchy/HierarchyOrdering.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine::Editor
{

namespace
{

bool Contains(std::span<const ECS::EntityHandle> entities, ECS::EntityHandle entity)
{
    return std::any_of(entities.begin(), entities.end(), [entity](const ECS::EntityHandle& e) { return e.id == entity.id; });
}

std::vector<HierarchyEntityState> CaptureStates(ECS::World& world,
                                                                       std::span<const ECS::EntityHandle> entities)
{
    std::vector<HierarchyEntityState> states;
    states.reserve(entities.size());
    for (const ECS::EntityHandle e : entities)
        states.push_back(CaptureReparentState(world, e));
    return states;
}

void ReparentKeepingWorldPose(ECS::World& world, ECS::EntityHandle entity, ECS::EntityHandle destParent)
{
    if (destParent.IsValid())
    {
        Components::Parent p{};
        p.Entity = destParent;
        world.AddComponentImmediate(entity, p);

        // Read after the add: adding Parent can move the entity to another archetype.
        const auto* parentWorld = world.GetComponent<Components::WorldTransform>(destParent);
        const auto* entityWorld = world.GetComponent<Components::WorldTransform>(entity);
        if (entityWorld && parentWorld)
        {
            const glm::mat4 newLocal = glm::inverse(glm::make_mat4(parentWorld->matrix)) * glm::make_mat4(entityWorld->matrix);
            Components::Transform xf{};
            std::memcpy(xf.matrix, &newLocal[0][0], sizeof(xf.matrix));
            world.AddComponentImmediate(entity, xf);
        }
        return;
    }

    // Moving to the root: the local transform becomes the current world transform.
    if (const auto* entityWorld = world.GetComponent<Components::WorldTransform>(entity))
    {
        Components::Transform xf{};
        std::memcpy(xf.matrix, entityWorld->matrix, sizeof(xf.matrix));
        world.AddComponentImmediate(entity, xf);
    }
    world.RemoveComponentImmediate<Components::Parent>(entity);
}

} // namespace

ECS::EntityHandle GetEffectiveParent(ECS::World& world, ECS::EntityHandle entity)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return {};
    if (const auto* p = world.GetComponent<Components::Parent>(entity))
    {
        if (p->Entity.IsValid() && world.IsValid(p->Entity))
            return p->Entity;
    }
    return {};
}

bool IsDescendantOf(ECS::World& world, ECS::EntityHandle node, ECS::EntityHandle possibleAncestor)
{
    if (!node.IsValid() || !possibleAncestor.IsValid())
        return false;
    if (!world.IsValid(node) || !world.IsValid(possibleAncestor))
        return false;

    ECS::EntityHandle cur = node;
    while (cur.IsValid() && world.IsValid(cur))
    {
        if (cur.id == possibleAncestor.id)
            return true;
        cur = GetEffectiveParent(world, cur);
    }
    return false;
}

HierarchyEntityState CaptureReparentState(ECS::World& world, ECS::EntityHandle entity)
{
    HierarchyEntityState s{};
    s.entity = entity;
    if (const auto* p = world.GetComponent<Components::Parent>(entity))
    {
        s.hadParent = p->Entity.IsValid() && world.IsValid(p->Entity);
        s.parent = *p;
    }
    if (const auto* o = world.GetComponent<Components::HierarchyOrder>(entity))
    {
        s.hadOrder = true;
        s.order = *o;
    }
    if (const auto* t = world.GetComponent<Components::Transform>(entity))
    {
        s.hadTransform = true;
        s.transform = *t;
    }
    return s;
}

HierarchyMoveStates MoveEntitiesKeepingWorldPose(ECS::World& world,
                                                 std::span<const ECS::EntityHandle> moved,
                                                 ECS::EntityHandle destParent,
                                                 std::size_t insertIndex)
{
    std::vector<ECS::EntityHandle> siblings = ChildrenInHierarchyOrder(world, destParent);
    std::erase_if(siblings, [moved](const ECS::EntityHandle& e) { return Contains(moved, e); });
    insertIndex = std::min(insertIndex, siblings.size());
    siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(insertIndex), moved.begin(), moved.end());

    // The renumbered siblings include every moved entity.
    std::vector<ECS::EntityHandle> affected = siblings;
    std::sort(affected.begin(), affected.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id < b.id; });
    affected.erase(std::unique(affected.begin(), affected.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b) { return a.id == b.id; }),
                   affected.end());

    HierarchyMoveStates states;
    states.Before = CaptureStates(world, affected);
    for (const ECS::EntityHandle e : moved)
        ReparentKeepingWorldPose(world, e, destParent);
    WriteSiblingOrder(world, siblings);
    states.After = CaptureStates(world, affected);
    return states;
}

} // namespace GameEngine::Editor
