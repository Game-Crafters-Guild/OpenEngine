#pragma once

#include "Components/Hierarchy.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Query.h"
#include "Types/Types.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;

// Derives the tag TDerived ("this entity carries TSelf, or an ancestor does") from the authored tag
// TSelf and the Parent chain. DisabledInHierarchySystem derives DisabledInHierarchy from Disabled with
// it; SceneViewHiddenInHierarchySystem derives the Scene View eye's inherited state the same way.
//
// It runs only when a structural change or a Parent edit could have moved the answer, and with no
// TSelf or TDerived in the world it exits on two archetype-level counts. It reads every row, disabled
// ones included: they are what it classifies. It mutates archetypes, so its owner runs exclusively.
template <class TSelf, class TDerived>
class HierarchyTagDerivation
{
  public:
    // Binds to `world` when it is not the world of the last call (the first call, or a world
    // swap) and returns true; returns false when already bound to it.
    bool BindIfNeeded(World& world);

    // Re-derives TDerived when the world's structure or a Parent column changed since the last call.
    void Update(World& world);

    // Tags added plus tags removed by the last update that ran, and the number of updates that
    // derived at all: the diagnostics the tests assert on.
    std::size_t GetLastPropagatedCount() const { return m_LastPropagatedCount; }
    std::size_t GetDerivationCount() const { return m_DerivationCount; }

  private:
    // One slot per entity index, valid for the run whose stamp it carries. Entity pins the slot to
    // a handle so a recycled index cannot be mistaken for the entity a stale Parent handle names.
    struct EntityState
    {
        uint32 Stamp = 0;
        EntityHandle Entity{};
        EntityHandle Parent{};
        uint32 Flags = 0;
    };

    enum StateFlags : uint32
    {
        kSelfTagged = 1u << 0,
        kHasTag = 1u << 1,
        kCandidate = 1u << 2,
        kResolved = 1u << 3,
        kInherited = 1u << 4,
        kVisiting = 1u << 5,
    };

    bool HasParentColumnChange(uint64 entryVersion);
    // Fills the per-entity slots from the Parent, TSelf and TDerived columns and returns the
    // entities worth resolving.
    void CollectCandidates(World& world);
    // Walks up the Parent chain, memoizing every entity it passes, so the whole candidate set costs
    // one pass over the edges rather than one per entity.
    bool ResolveInherited(EntityHandle entity);
    EntityState* FindState(EntityHandle entity);
    EntityState& TouchState(EntityHandle entity);
    void MarkCandidate(EntityState& state, EntityHandle entity);

    uint64 m_WorldId = 0;
    bool m_Bound = false;

    std::optional<Query<Read<Components::Parent>>> m_ParentQuery;
    // A Changed<> gate lives on the query object it is set on, so the reparent probe keeps its own
    // instance rather than filtering the collection pass.
    std::optional<Query<Read<Components::Parent>>> m_ParentChangeQuery;
    std::optional<Query<Read<TSelf>>> m_SelfQuery;
    std::optional<Query<Read<TDerived>>> m_TaggedQuery;

    std::size_t m_StructuralVersion = 0;
    bool m_StructuralVersionValid = false;
    ChangeGate m_ParentGate;
    bool m_ParentGateValid = false;

    // Scratch retained across frames: a steady-state run allocates nothing.
    std::vector<EntityState> m_States;
    std::vector<EntityHandle> m_Candidates;
    std::vector<uint32> m_Chain;
    std::vector<EntityHandle> m_ToTag;
    std::vector<EntityHandle> m_ToUntag;
    uint32 m_RunStamp = 0;
    std::size_t m_LastPropagatedCount = 0;
    std::size_t m_DerivationCount = 0;
};

} // namespace ECS
} // namespace GameEngine
