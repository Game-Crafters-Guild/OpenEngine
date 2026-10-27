#include "Picking/PickRootResolver.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Scene/SceneRaycast.h"

namespace GameEngine::Editor::Picking
{

namespace
{

using ChildrenByParent =
    std::unordered_map<ECS::EntityHandle, std::vector<ECS::EntityHandle>, ECS::EntityHandleHash>;

// Every (parent -> children) edge, in one O(parented entities) pass. The resolve walk builds it at
// most once, when it reaches a container node; an entity under a climb root climbs in O(depth).
void BuildChildrenMap(ECS::World& world, ChildrenByParent& out)
{
    world.Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
        [&out](ECS::EntityHandle e, const Components::Parent& p)
        {
            if (p.Entity.IsValid())
                out[p.Entity].push_back(e);
        });
}

// True when nothing in `root`'s subtree renders an asset other than `model`.
// Lets the root walk climb through Transform-only container and helper nodes
// that belong to one model instance, while refusing to climb into grouping
// nodes that also contain other content.
bool SubtreeRendersSingleModel(ECS::World& world,
                               const ChildrenByParent& children,
                               ECS::EntityHandle root,
                               const GUID& model)
{
    // Reused across calls so hover/click resolves don't heap-allocate the
    // traversal stack each time (picking runs on the main thread; thread_local
    // keeps any future off-thread use safe).
    thread_local std::vector<ECS::EntityHandle> frontier;
    frontier.clear();
    frontier.push_back(root);
    while (!frontier.empty())
    {
        const ECS::EntityHandle current = frontier.back();
        frontier.pop_back();

        // Components registered as their own pick target (plugin traits) stop
        // the model-instance grouping: their entity never folds into a root.
        const auto registered = EditorComponentTraitsRegistry::Get().Snapshot();
        for (const auto& [typeId, traits] : *registered)
        {
            if (traits.IsPickInstanceRoot && world.HasComponent(current, typeId))
                return false;
        }
        const auto* mr = world.GetComponent<Components::MeshRenderer>(current);
        if (mr && !(mr->modelAssetGuid.ToGuid() == model))
            return false;

        const auto it = children.find(current);
        if (it != children.end())
            frontier.insert(frontier.end(), it->second.begin(), it->second.end());
    }
    return true;
}

// The outermost entity from `entity` up its parent chain that carries a
// component registered IsPickClimbRoot; invalid when none does. O(depth).
ECS::EntityHandle FindOutermostClimbRoot(ECS::World& world, ECS::EntityHandle entity)
{
    thread_local std::vector<ECS::ComponentTypeId> climbTypes;
    climbTypes.clear();
    const auto registered = EditorComponentTraitsRegistry::Get().Snapshot();
    for (const auto& [typeId, traits] : *registered)
    {
        if (traits.IsPickClimbRoot)
            climbTypes.push_back(typeId);
    }
    if (climbTypes.empty())
        return {};

    ECS::EntityHandle outermost{};
    for (ECS::EntityHandle current = entity; current.IsValid() && world.IsValid(current);)
    {
        for (const ECS::ComponentTypeId typeId : climbTypes)
        {
            if (world.HasComponent(current, typeId))
            {
                outermost = current;
                break;
            }
        }
        const auto* parent = world.GetComponent<Components::Parent>(current);
        current = parent ? parent->Entity : ECS::EntityHandle{};
    }
    return outermost;
}

} // namespace

ECS::EntityHandle ResolvePickRoot(ECS::World& world, ECS::EntityHandle entity)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return entity;

    const ECS::EntityHandle climbRoot = FindOutermostClimbRoot(world, entity);
    if (climbRoot.IsValid())
        return climbRoot;

    const auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return entity;

    // Primitives and unassigned renderers carry no instance identity; a null
    // GUID would merge unrelated hand-placed primitives, so pick them exactly.
    const GUID model = mr->modelAssetGuid.ToGuid();
    if (model.IsNull())
        return entity;

    // Reused across resolves: clear() keeps the bucket array, so repeat
    // resolves avoid the map's rehash allocations (per-key child vectors are
    // still rebuilt — acceptable, the map is only built when the walk reaches
    // a container node at all).
    thread_local ChildrenByParent children;
    bool childrenBuilt = false;

    for (;;)
    {
        const auto* parentComp = world.GetComponent<Components::Parent>(entity);
        if (!parentComp || !parentComp->Entity.IsValid() || !world.IsValid(parentComp->Entity))
            break;

        const ECS::EntityHandle parent = parentComp->Entity;
        const auto* parentMr = world.GetComponent<Components::MeshRenderer>(parent);
        bool sameInstance = false;
        if (parentMr)
        {
            sameInstance = (parentMr->modelAssetGuid.ToGuid() == model);
        }
        else
        {
            if (!childrenBuilt)
            {
                children.clear();
                BuildChildrenMap(world, children);
                childrenBuilt = true;
            }
            sameInstance = SubtreeRendersSingleModel(world, children, parent, model);
        }
        if (!sameInstance)
            break;
        entity = parent;
    }
    return entity;
}

void BuildPickCandidates(ECS::World& world,
                         const std::vector<Scene::PickHit>& sortedHits,
                         bool exactPick,
                         std::vector<ECS::EntityHandle>& outCandidates)
{
    outCandidates.clear();
    if (sortedHits.empty())
        return;

    outCandidates.reserve(sortedHits.size() + 1);
    if (!exactPick)
    {
        const ECS::EntityHandle root = ResolvePickRoot(world, sortedHits.front().Entity);
        if (root.IsValid())
            outCandidates.push_back(root);
    }
    for (const Scene::PickHit& hit : sortedHits)
    {
        if (std::find(outCandidates.begin(), outCandidates.end(), hit.Entity) == outCandidates.end())
            outCandidates.push_back(hit.Entity);
    }
}

} // namespace GameEngine::Editor::Picking
