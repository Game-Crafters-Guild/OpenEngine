#include "ECS/HierarchyTagDerivation.h"

#include "Components/Hierarchy.h"
#include "Components/SceneViewHidden.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

namespace GameEngine
{
namespace ECS
{

using GameEngine::Components::Parent;

template <class TSelf, class TDerived>
bool HierarchyTagDerivation<TSelf, TDerived>::BindIfNeeded(World& world)
{
    if (m_Bound && m_WorldId == world.GetWorldId())
        return false;

    m_WorldId = world.GetWorldId();
    m_Bound = true;
    m_StructuralVersionValid = false;
    m_ParentGateValid = false;
    m_States.clear();

    // Every pass has to see the rows the enable model hides: they are classified too.
    m_ParentQuery.emplace(&world);
    m_ParentQuery->IncludeDisabled();
    m_ParentChangeQuery.emplace(&world);
    m_ParentChangeQuery->IncludeDisabled();
    m_SelfQuery.emplace(&world);
    m_SelfQuery->IncludeDisabled();
    m_TaggedQuery.emplace(&world);
    m_TaggedQuery->IncludeDisabled();
    return true;
}

template <class TSelf, class TDerived>
bool HierarchyTagDerivation<TSelf, TDerived>::HasParentColumnChange(uint64 entryVersion)
{
    bool changed = false;
    if (ChangeFilter::Enabled())
    {
        if (!m_ParentGateValid)
            m_ParentGate.LastRunVersion = 0; // see everything once
        m_ParentChangeQuery->Changed<Parent>(m_ParentGate);
        m_ParentChangeQuery->BatchEach([&](const Parent*, std::size_t) { changed = true; });
    }
    else
    {
        // Diagnostic filter-off mode has no per-column stamps to read, so the
        // pass runs whenever anything is tagged at all.
        changed = true;
    }

    m_ParentGate.LastRunVersion = entryVersion;
    m_ParentGateValid = true;
    return changed;
}

template <class TSelf, class TDerived>
typename HierarchyTagDerivation<TSelf, TDerived>::EntityState*
HierarchyTagDerivation<TSelf, TDerived>::FindState(EntityHandle entity)
{
    if (!entity.IsValid() || entity.index >= m_States.size())
        return nullptr;
    EntityState& state = m_States[entity.index];
    if (state.Stamp != m_RunStamp || state.Entity != entity)
        return nullptr;
    return &state;
}

template <class TSelf, class TDerived>
typename HierarchyTagDerivation<TSelf, TDerived>::EntityState&
HierarchyTagDerivation<TSelf, TDerived>::TouchState(EntityHandle entity)
{
    EntityState& state = m_States[entity.index];
    if (state.Stamp != m_RunStamp || state.Entity != entity)
    {
        state.Stamp = m_RunStamp;
        state.Entity = entity;
        state.Parent = EntityHandle{};
        state.Flags = 0;
    }
    return state;
}

template <class TSelf, class TDerived>
void HierarchyTagDerivation<TSelf, TDerived>::MarkCandidate(EntityState& state, EntityHandle entity)
{
    if ((state.Flags & kCandidate) == 0)
    {
        state.Flags |= kCandidate;
        m_Candidates.push_back(entity);
    }
}

template <class TSelf, class TDerived>
void HierarchyTagDerivation<TSelf, TDerived>::CollectCandidates(World& world)
{
    m_Candidates.clear();
    m_States.resize(world.GetEntityIndexBound());

    m_ParentQuery->Each(
        [&](EntityHandle entity, const Parent& parent)
        {
            EntityState& state = TouchState(entity);
            state.Parent = parent.parent;
            MarkCandidate(state, entity);
        });

    m_SelfQuery->Each(
        [&](EntityHandle entity, const TSelf&)
        {
            EntityState& state = TouchState(entity);
            state.Flags |= kSelfTagged;
            MarkCandidate(state, entity);
        });

    m_TaggedQuery->Each(
        [&](EntityHandle entity, const TDerived&)
        {
            EntityState& state = TouchState(entity);
            state.Flags |= kHasTag;
            MarkCandidate(state, entity);
        });
}

template <class TSelf, class TDerived>
bool HierarchyTagDerivation<TSelf, TDerived>::ResolveInherited(EntityHandle entity)
{
    m_Chain.clear();
    bool inherited = false;

    for (EntityHandle current = entity;;)
    {
        EntityState* state = FindState(current);
        if (!state)
            break; // no slot: a root this run never saw, or a stale parent handle
        if ((state->Flags & kResolved) != 0)
        {
            inherited = (state->Flags & kInherited) != 0;
            break;
        }
        if ((state->Flags & kVisiting) != 0)
            break; // a Parent cycle: the entity that closed it counts as a root
        state->Flags |= kVisiting;
        m_Chain.push_back(current.index);
        if ((state->Flags & kSelfTagged) != 0)
        {
            inherited = true;
            break;
        }
        if (!state->Parent.IsValid())
            break;
        current = state->Parent;
    }

    // Nothing on the chain was self-tagged except possibly its last entry, so
    // the whole chain shares the answer that entry produced.
    for (uint32 index : m_Chain)
    {
        EntityState& state = m_States[index];
        state.Flags &= ~kVisiting;
        state.Flags |= kResolved;
        if (inherited)
            state.Flags |= kInherited;
        else
            state.Flags &= ~kInherited;
    }
    return inherited;
}

template <class TSelf, class TDerived>
void HierarchyTagDerivation<TSelf, TDerived>::Update(World& world)
{
    BindIfNeeded(world);

    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const bool structuralChange =
        !m_StructuralVersionValid || m_StructuralVersion != structuralVersion;

    // A reparent is a data write to an existing Parent column, so it never
    // bumps the structural version; the change filter is what catches it.
    const bool parentChange = !structuralChange && HasParentColumnChange(entryVersion);
    if (structuralChange)
    {
        m_ParentGate.LastRunVersion = entryVersion;
        m_ParentGateValid = true;
    }
    if (!structuralChange && !parentChange)
        return;

    m_LastPropagatedCount = 0;
    m_StructuralVersion = structuralVersion;
    m_StructuralVersionValid = true;

    // Nothing tagged anywhere: no tag can be owed and none can be stale.
    // Both counts are archetype-level sums over a cached list.
    if (m_SelfQuery->Count() == 0 && m_TaggedQuery->Count() == 0)
        return;

    if (++m_RunStamp == 0)
    {
        for (EntityState& state : m_States)
            state.Stamp = 0;
        m_RunStamp = 1;
    }

    ++m_DerivationCount;
    CollectCandidates(world);

    m_ToTag.clear();
    m_ToUntag.clear();
    for (EntityHandle entity : m_Candidates)
    {
        const bool inherited = ResolveInherited(entity);
        EntityState* state = FindState(entity);
        const bool hasTag = state != nullptr && (state->Flags & kHasTag) != 0;
        if (inherited && !hasTag)
            m_ToTag.push_back(entity);
        else if (!inherited && hasTag)
            m_ToUntag.push_back(entity);
    }

    // Mutating archetypes invalidates the iteration above, so it happens only
    // once every pass has read what it needs.
    for (EntityHandle entity : m_ToTag)
        world.AddComponentImmediate<TDerived>(entity, TDerived{});
    for (EntityHandle entity : m_ToUntag)
        world.RemoveComponentImmediate<TDerived>(entity);

    m_LastPropagatedCount = m_ToTag.size() + m_ToUntag.size();

    // The pass just made structural changes of its own; adopt the version they
    // produced so the next update is not woken by this one's work. Those moves
    // also stamped every column of the chunks they touched, Parent included, so
    // the Parent gate is resampled here too. An end-of-run sample is sound only
    // because the owner runs exclusively: nothing else writes Parent while it
    // runs, so no writer's stamp can fall between the sample and the return.
    m_StructuralVersion = world.GetStructuralChangeVersion();
    m_ParentGate.LastRunVersion = world.GetGlobalSystemVersion();
}

// The tag pairs the engine derives.
template class HierarchyTagDerivation<Disabled, DisabledInHierarchy>;
template class HierarchyTagDerivation<Components::SceneViewHidden, Components::SceneViewHiddenInHierarchy>;

} // namespace ECS
} // namespace GameEngine
