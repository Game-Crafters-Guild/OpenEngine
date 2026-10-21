#include "Editor/Hierarchy/HierarchyOrdering.h"

#include "Editor/Hierarchy/HierarchyReparent.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <vector>

namespace GameEngine {
namespace Editor {

namespace {

std::int32_t MaxHierarchyOrder(ECS::World& world)
{
    std::int32_t maxOrder = 0;
    std::vector<ECS::EntityHandle> alive;
    world.GetAliveEntitiesSnapshot(alive);
    for (const ECS::EntityHandle ent : alive)
    {
        if (!world.IsValid(ent))
            continue;
        if (const auto* o = world.GetComponent<Components::HierarchyOrder>(ent))
            maxOrder = std::max(maxOrder, o->order);
    }
    return maxOrder;
}

std::int32_t OrderOrDefault(ECS::World& world, ECS::EntityHandle e)
{
    if (!e.IsValid() || !world.IsValid(e))
        return 0;
    if (const auto* o = world.GetComponent<Components::HierarchyOrder>(e))
        return o->order;
    return 0;
}

bool PrecedesAmongSiblings(ECS::World& world, ECS::EntityHandle a, ECS::EntityHandle b)
{
    const std::int32_t oa = OrderOrDefault(world, a);
    const std::int32_t ob = OrderOrDefault(world, b);
    if (oa != ob)
        return oa < ob;
    return a.id < b.id;
}

// The entity and its ancestors, the topmost first.
std::vector<ECS::EntityHandle> PathFromRoot(ECS::World& world, ECS::EntityHandle entity)
{
    std::vector<ECS::EntityHandle> path;
    for (ECS::EntityHandle e = entity; e.IsValid(); e = GetEffectiveParent(world, e))
        path.push_back(e);
    std::reverse(path.begin(), path.end());
    return path;
}

} // namespace

Components::HierarchyOrder NextHierarchyOrderAtBottom(ECS::World* world)
{
    Components::HierarchyOrder ho{};
    if (!world)
        return ho;
    ho.order = MaxHierarchyOrder(*world) + 1;
    return ho;
}

void AppendUnorderedRootsToHierarchyEnd(ECS::World& world, std::span<const ECS::EntityHandle> roots)
{
    std::int32_t next = MaxHierarchyOrder(world) + 1;
    for (const ECS::EntityHandle root : roots)
    {
        if (!root.IsValid() || !world.IsValid(root))
            continue;
        if (world.GetComponent<Components::HierarchyOrder>(root))
            continue;
        Components::HierarchyOrder ho{};
        ho.order = next++;
        world.AddComponentImmediate(root, ho);
    }
}

void SortByHierarchyOrder(ECS::World& world, std::vector<ECS::EntityHandle>& entities)
{
    std::sort(entities.begin(), entities.end(), [&world](const ECS::EntityHandle& a, const ECS::EntityHandle& b) {
        return PrecedesAmongSiblings(world, a, b);
    });
}

bool PrecedesInHierarchyTree(ECS::World& world, ECS::EntityHandle a, ECS::EntityHandle b)
{
    const std::vector<ECS::EntityHandle> pathA = PathFromRoot(world, a);
    const std::vector<ECS::EntityHandle> pathB = PathFromRoot(world, b);
    const std::size_t shared = std::min(pathA.size(), pathB.size());
    for (std::size_t i = 0; i < shared; ++i)
    {
        if (pathA[i].id != pathB[i].id)
            return PrecedesAmongSiblings(world, pathA[i], pathB[i]);
    }
    return pathA.size() < pathB.size();
}

std::vector<ECS::EntityHandle> ChildrenInHierarchyOrder(ECS::World& world, ECS::EntityHandle parent)
{
    std::vector<ECS::EntityHandle> children;
    for (auto* arch : world.GetAllArchetypes())
    {
        if (!arch)
            continue;
        for (const ECS::EntityHandle e : arch->CollectEntities())
        {
            if (!e.IsValid() || !world.IsValid(e))
                continue;
            if (GetEffectiveParent(world, e).id == parent.id)
                children.push_back(e);
        }
    }
    SortByHierarchyOrder(world, children);
    return children;
}

void WriteSiblingOrder(ECS::World& world, std::span<const ECS::EntityHandle> siblings)
{
    for (std::size_t i = 0; i < siblings.size(); ++i)
    {
        const ECS::EntityHandle e = siblings[i];
        if (!e.IsValid() || !world.IsValid(e))
            continue;
        Components::HierarchyOrder o{};
        o.order = static_cast<std::int32_t>(i * 10);
        world.AddComponentImmediate(e, o);
    }
}

} // namespace Editor
} // namespace GameEngine
