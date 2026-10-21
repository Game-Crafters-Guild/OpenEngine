#include "Editor/Hierarchy/HierarchyEmptyParent.h"

#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "Editor/Hierarchy/HierarchyReparent.h"
#include "Editor/Hierarchy/HierarchyReparentRules.h"
#include "UndoRedo/CreateEmptyParentCommand.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

// The live entities that are not below another one given, in Hierarchy tree order.
std::vector<ECS::EntityHandle> TopmostInTreeOrder(ECS::World& world, std::span<const ECS::EntityHandle> entities)
{
    std::vector<ECS::EntityHandle> live;
    std::unordered_set<std::uint32_t> seen;
    for (const ECS::EntityHandle e : entities)
    {
        if (e.IsValid() && world.IsValid(e) && seen.insert(e.id).second)
            live.push_back(e);
    }

    std::vector<ECS::EntityHandle> topmost;
    for (const ECS::EntityHandle e : live)
    {
        const bool belowAnother = std::any_of(live.begin(), live.end(), [&](const ECS::EntityHandle& other) {
            return other.id != e.id && IsDescendantOf(world, e, other);
        });
        if (!belowAnother)
            topmost.push_back(e);
    }
    std::sort(topmost.begin(), topmost.end(), [&world](const ECS::EntityHandle& a, const ECS::EntityHandle& b) {
        return PrecedesInHierarchyTree(world, a, b);
    });
    return topmost;
}

// The deepest entity every one of `entities` is below ({} = the scene root).
ECS::EntityHandle LowestCommonAncestor(ECS::World& world, std::span<const ECS::EntityHandle> entities)
{
    ECS::EntityHandle ancestor = GetEffectiveParent(world, entities.front());
    for (const ECS::EntityHandle e : entities)
    {
        const ECS::EntityHandle parent = GetEffectiveParent(world, e);
        while (ancestor.IsValid() && !IsDescendantOf(world, parent, ancestor))
            ancestor = GetEffectiveParent(world, ancestor);
    }
    return ancestor;
}

// The child of `ancestor` that `entity` is, or is below.
ECS::EntityHandle BranchUnder(ECS::World& world, ECS::EntityHandle ancestor, ECS::EntityHandle entity)
{
    ECS::EntityHandle branch = entity;
    while (GetEffectiveParent(world, branch).id != ancestor.id)
        branch = GetEffectiveParent(world, branch);
    return branch;
}

Components::Transform TranslationTransform(float x, float y, float z)
{
    Components::Transform t{};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
    std::copy(std::begin(identity), std::end(identity), t.matrix);
    return t;
}

// The centre of the entities' world positions; entities without a WorldTransform are left out.
Components::Transform CentreOfWorldPositions(ECS::World& world, std::span<const ECS::EntityHandle> entities)
{
    float sum[3] = {0.0f, 0.0f, 0.0f};
    int count = 0;
    for (const ECS::EntityHandle e : entities)
    {
        if (const auto* wt = world.GetComponent<Components::WorldTransform>(e))
        {
            sum[0] += wt->matrix[12];
            sum[1] += wt->matrix[13];
            sum[2] += wt->matrix[14];
            ++count;
        }
    }
    if (count == 0)
        return TranslationTransform(0.0f, 0.0f, 0.0f);
    return TranslationTransform(sum[0] / count, sum[1] / count, sum[2] / count);
}

// Appends the states of `from` for entities `into` does not hold yet.
void AppendNewStates(std::vector<HierarchyEntityState>& into,
                     const std::vector<HierarchyEntityState>& from)
{
    for (const auto& s : from)
    {
        const bool held = std::any_of(into.begin(), into.end(), [&s](const auto& h) { return h.entity.id == s.entity.id; });
        if (!held)
            into.push_back(s);
    }
}

} // namespace

EmptyParentResult CreateEmptyParent(ECS::World& world,
                                    EditorChangeNotifications* notifications,
                                    std::span<const ECS::EntityHandle> entities,
                                    const Components::Name& name)
{
    const std::vector<ECS::EntityHandle> grouped = TopmostInTreeOrder(world, entities);
    if (grouped.empty())
        return {};

    const ECS::EntityHandle destParent = LowestCommonAncestor(world, grouped);
    if (!CanAcceptAuthoredChildren(world, destParent))
        return {};

    // Siblings of the new parent, found before it exists so it cannot be among them.
    const std::vector<ECS::EntityHandle> siblings = ChildrenInHierarchyOrder(world, destParent);
    const ECS::EntityHandle anchor = BranchUnder(world, destParent, grouped.front());
    const auto anchorAt = std::find_if(siblings.begin(), siblings.end(), [anchor](const ECS::EntityHandle& e) { return e.id == anchor.id; });
    const std::size_t insertIndex = static_cast<std::size_t>(anchorAt - siblings.begin());

    const ECS::EntityHandle newParent = world.CreateEntity();
    if (!newParent.IsValid())
        return {};

    // Created as a root at its world pose. Its WorldTransform is written here too:
    // the moves below read it, and the transform system would only write it next frame.
    const Components::Transform pose = CentreOfWorldPositions(world, grouped);
    world.AddComponentImmediate(newParent, name);
    world.AddComponentImmediate(newParent, pose);
    Components::WorldTransform poseWorld{};
    std::copy(std::begin(pose.matrix), std::end(pose.matrix), poseWorld.matrix);
    world.AddComponentImmediate(newParent, poseWorld);

    const ECS::EntityHandle placed[] = {newParent};
    HierarchyMoveStates place = MoveEntitiesKeepingWorldPose(world, placed, destParent, insertIndex);
    HierarchyMoveStates group = MoveEntitiesKeepingWorldPose(world, grouped, newParent, 0);

    // Each entity's first state before the two moves, and its last after them.
    std::vector<HierarchyEntityState> before = std::move(place.Before);
    AppendNewStates(before, group.Before);
    std::vector<HierarchyEntityState> after = std::move(group.After);
    AppendNewStates(after, place.After);

    EmptyParentResult result;
    result.NewParent = newParent;
    result.Command = std::make_unique<CreateEmptyParentCommand>(
        "Create Empty Parent", &world, notifications, newParent, name, std::move(before), std::move(after));
    return result;
}

} // namespace GameEngine::Editor
