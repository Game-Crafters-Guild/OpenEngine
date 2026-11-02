#include "Editor/Hierarchy/HierarchyDataProvider.h"

#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/Skybox.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>

namespace GameEngine
{

bool HierarchyDataProvider::IsListed(const World& world, const EntityHandle& e)
{
    return !world.HasComponent<Components::PhysicsWorldSettingsComponent>(e) &&
           !world.HasComponent<Components::RuntimeOnlyEntity>(e);
}

HierarchyDataProvider::EntityHandle HierarchyDataProvider::ListedSelfOrAncestor(const World& world, EntityHandle e)
{
    while (e.IsValid() && world.IsValid(e) && !IsListed(world, e))
    {
        const auto* parent = world.GetComponent<Parent>(e);
        e = parent ? parent->Entity : EntityHandle{};
    }
    return e.IsValid() && world.IsValid(e) ? e : EntityHandle{};
}

TreeId HierarchyDataProvider::ListedRowIds(const World& world, std::span<const EntityHandle> picks, std::vector<TreeId>& ids)
{
    ids.clear();
    ids.reserve(picks.size());
    bool anyMapped = false;
    for (const EntityHandle& pick : picks)
    {
        const EntityHandle shown = ListedSelfOrAncestor(world, pick);
        if (!shown.IsValid())
            continue;

        const TreeId tid = Encode(shown);
        if (tid == 0 || tid == kRootId)
            continue;

        anyMapped = anyMapped || shown != pick;
        ids.push_back(tid);
    }
    if (ids.empty())
        return 0;

    // Only a pick mapped to an ancestor can repeat a row, so only then sort once to remove repeats.
    const TreeId anchor = ids.front();
    if (anyMapped)
    {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    return anchor;
}

void HierarchyDataProvider::SetWorld(World* world)
{
    m_World = world;
    Rebuild();
}

void HierarchyDataProvider::Rebuild()
{
    m_Roots.clear();
    m_Children.clear();
    m_EntityParentKey.clear();
    m_PendingResortParents.clear(); // a full rebuild resorts everything; drop stale deferrals
    if (!m_World)
    {
        m_Labels.clear();
        return;
    }
    // m_Labels is intentionally retained across rebuilds: strings are reused in place
    // below (assign keeps each buffer) and stale entries are pruned at the end.

    // Collect alive entities from the World's canonical metadata.
    // This avoids observing transient archetype-move states and eliminates the need for
    // defensive de-duplication when components are added/removed immediately.
    m_World->GetAliveEntitiesSnapshot(m_SnapshotScratch);

    m_SnapshotScratch.erase(
        std::remove_if(m_SnapshotScratch.begin(), m_SnapshotScratch.end(), [this](const EntityHandle& e)
                       { return !IsListed(*m_World, e); }),
        m_SnapshotScratch.end());

    // Stable pre-sort by TreeId (Encode). The decorate-sort below (SortGroup) applies a
    // total order with an id tie-break, so this only fixes the flatten-path order
    // deterministically; final sibling order is fully determined by SortGroup.
    std::sort(m_SnapshotScratch.begin(), m_SnapshotScratch.end(), [](const EntityHandle& a, const EntityHandle& b)
              { return Encode(a) < Encode(b); });

    // Build membership set (use the same packed representation as TreeId)
    std::unordered_set<uint32_t> alive;
    alive.reserve(m_SnapshotScratch.size());
    for (auto& e : m_SnapshotScratch)
    {
        alive.insert(static_cast<uint32_t>(Encode(e)));
    }

    // Build adjacency from Parent component
    bool anyParent = false;
    for (auto& e : m_SnapshotScratch)
    {
        ECS::Entity ent(m_World, e);
        if (auto* p = ent.Get<Parent>())
        {
            anyParent = true;

            // Key children by the same packed representation we expose via TreeId
            TreeId parentKey = Encode(p->Entity);
            if (p->Entity.IsValid() && alive.count(static_cast<uint32_t>(parentKey)))
            {
                m_Children[static_cast<uint32_t>(parentKey)].push_back(e);
            }
            else
            {
                m_Roots.push_back(e);
            }
        }
        else
        {
            m_Roots.push_back(e);
        }
    }

    // No Parent anywhere: show every entity as a root (still ordered by HierarchyOrder below).
    if (!anyParent)
    {
        m_Children.clear();
        m_Roots = m_SnapshotScratch;
    }

    // Sibling order: either scene-authored (HierarchyOrder) or overridden by the
    // view-only sort mode configured on the panel. The decorate-sort keys and comparator
    // (KeyOf / KeyLess, via SortGroup) are the SAME ones the incremental sorted-inserts
    // use — comparator parity is the correctness crux, so both paths share one helper.
    SortGroup(m_Roots);
    for (auto& kv : m_Children)
        SortGroup(kv.second);

    // Labels: prefer Name component if present, fall back to entity id. Reuse the existing
    // label strings across rebuilds (assign into the slot keeps its buffer) and prune
    // entries whose entity is no longer alive, so a rebuild does not free and reallocate one
    // string per entity every time.
    for (const EntityHandle& e : m_SnapshotScratch)
        BuildLabel(e);
    for (auto it = m_Labels.begin(); it != m_Labels.end();)
    {
        if (alive.count(static_cast<uint32_t>(it->first)) == 0)
            it = m_Labels.erase(it);
        else
            ++it;
    }

    // Seed the incremental position index from the freshly-built adjacency so the per-entity
    // ops and the identity diff have correct bookkeeping to mutate.
    SeedParentKeyIndex();

    MarkAllChanged();
}

HierarchyDataProvider::DiffOutcome HierarchyDataProvider::ApplyIdentityDiff()
{
    DiffOutcome out;
    if (!m_World)
        return out;

    m_World->GetAliveEntitiesSnapshot(m_SnapshotScratch);

    // created = live keys not yet tracked. The hide filter is deferred to ApplyCreated
    // (only new entities need the component read), keeping this pass free of reads.
    m_DiffCreated.clear();
    std::unordered_set<uint32_t> snapKeys;
    snapKeys.reserve(m_SnapshotScratch.size());
    for (const EntityHandle& e : m_SnapshotScratch)
    {
        const uint32_t key = static_cast<uint32_t>(Encode(e));
        snapKeys.insert(key);
        if (m_EntityParentKey.find(key) == m_EntityParentKey.end())
            m_DiffCreated.push_back(e);
    }

    // destroyed = tracked keys no longer live. Removed handles are IDs only — never
    // dereferenced (ApplyDestroyed works purely on provider state).
    m_DiffDestroyed.clear();
    for (const auto& kv : m_EntityParentKey)
        if (snapKeys.find(kv.first) == snapKeys.end())
            m_DiffDestroyed.push_back(Decode(static_cast<TreeId>(kv.first)));

    out.AnyMembershipChange = !m_DiffCreated.empty() || !m_DiffDestroyed.empty();
    out.AnyDestroyed = !m_DiffDestroyed.empty();

    // F1: bail to a full Rebuild for large deltas (mass spawn/despawn). Measure-only up to
    // here (no structures mutated), so nothing to unwind.
    if (m_DiffCreated.size() + m_DiffDestroyed.size() > kIncrementalDeltaMax)
    {
        out.Escalate = true;
        return out;
    }

    for (const EntityHandle& e : m_DiffDestroyed)
        ApplyDestroyed(e);
    for (const EntityHandle& e : m_DiffCreated)
        ApplyCreated(e);
    // Intra-batch parent adoption: a child created before its (also-new) parent was
    // rooted during ApplyCreated because the parent wasn't tracked yet. Now that the
    // whole created set is present, re-home any such child. ApplyReparented no-ops when
    // placement is already correct, so pre-existing correctly-placed creates cost nothing.
    for (const EntityHandle& e : m_DiffCreated)
        ApplyReparented(e);

    return out;
}

void HierarchyDataProvider::ApplyCreated(const EntityHandle& e)
{
    if (!m_World || !e.IsValid())
        return;
    const uint32_t childKey = static_cast<uint32_t>(Encode(e));
    if (m_EntityParentKey.find(childKey) != m_EntityParentKey.end())
        return; // already tracked (C6 / re-delivered)

    if (!IsListed(*m_World, e))
        return;

    const TreeId parentKey = DesiredParentKey(e);
    if (parentKey == kRootId)
        SortedInsert(m_Roots, e);
    else
        SortedInsert(m_Children[static_cast<uint32_t>(parentKey)], e);
    m_EntityParentKey[childKey] = parentKey;
    BuildLabel(e);
    MarkStructureChanged();
}

void HierarchyDataProvider::ApplyDestroyed(const EntityHandle& e)
{
    const uint32_t childKey = static_cast<uint32_t>(Encode(e));
    auto itPos = m_EntityParentKey.find(childKey);
    if (itPos == m_EntityParentKey.end())
        return; // not tracked — idempotent
    const TreeId storedParent = itPos->second; // copy: the map may rehash below

    // Remove from its own sibling list.
    RemoveFromSiblingList(storedParent, e);

    // Re-root its children: dangling Parent ⇒ root (matches Rebuild's alive-check). Their
    // Parent components still point at the dead handle — identical to today's post-rebuild
    // state.
    auto childrenIt = m_Children.find(childKey);
    if (childrenIt != m_Children.end())
    {
        std::vector<EntityHandle> orphans = std::move(childrenIt->second);
        m_Children.erase(childrenIt);
        for (const EntityHandle& c : orphans)
        {
            SortedInsert(m_Roots, c);
            m_EntityParentKey[static_cast<uint32_t>(Encode(c))] = kRootId;
        }
    }

    m_EntityParentKey.erase(childKey);
    m_Labels.erase(Encode(e));
    MarkStructureChanged();
}

bool HierarchyDataProvider::ApplyReparented(const EntityHandle& e)
{
    if (!m_World)
        return false;
    const uint32_t childKey = static_cast<uint32_t>(Encode(e));
    auto itPos = m_EntityParentKey.find(childKey);
    if (itPos == m_EntityParentKey.end())
        return false; // destroyed / hidden / never tracked — idempotent no-op
    const TreeId storedParent = itPos->second;
    const TreeId desiredParent = DesiredParentKey(e); // absent Parent ⇒ root (Removed<Parent>)
    if (desiredParent == storedParent)
        return false;

    RemoveFromSiblingList(storedParent, e);
    if (desiredParent == kRootId)
        SortedInsert(m_Roots, e);
    else
        SortedInsert(m_Children[static_cast<uint32_t>(desiredParent)], e);
    m_EntityParentKey[childKey] = desiredParent;
    MarkStructureChanged();
    return true;
}

void HierarchyDataProvider::ApplyRenamed(const EntityHandle& e)
{
    if (!m_World)
        return;
    const uint32_t childKey = static_cast<uint32_t>(Encode(e));
    auto itPos = m_EntityParentKey.find(childKey);
    if (itPos == m_EntityParentKey.end())
        return; // not tracked — idempotent

    const TreeId tid = Encode(e);
    ComputeLabel(e, m_LabelScratch);
    auto lit = m_Labels.find(tid);
    const bool changed = (lit == m_Labels.end()) || (lit->second != m_LabelScratch);
    if (!changed)
        return; // F2: no actual label change (filters the chunk-stamp over-report)
    if (lit != m_Labels.end())
        lit->second = m_LabelScratch;
    else
        m_Labels.emplace(tid, m_LabelScratch);

    if (m_SortMode == HierarchySortMode::Custom)
    {
        MarkChanged(tid); // name-independent order: paint-only rebind
        return;
    }
    // Name-sensitive sort: defer the resort of the touched sibling list (coalesced across all
    // renames this Update by FlushPendingResorts — many renames into one list resort once).
    m_PendingResortParents.insert(itPos->second);
    MarkStructureChanged();
}

void HierarchyDataProvider::FlushPendingResorts()
{
    if (m_PendingResortParents.empty())
        return;
    for (TreeId parentKey : m_PendingResortParents)
    {
        if (parentKey == kRootId)
            SortGroup(m_Roots);
        else if (auto cit = m_Children.find(static_cast<uint32_t>(parentKey)); cit != m_Children.end())
            SortGroup(cit->second);
    }
    m_PendingResortParents.clear();
}

#if !defined(NDEBUG) && !defined(GE_DEBUGFAST)
void HierarchyDataProvider::DebugVerifyAgainstRebuild()
{
    std::vector<EntityHandle> incRoots = m_Roots;
    std::unordered_map<uint32_t, std::vector<EntityHandle>> incChildren = m_Children;
    std::unordered_map<TreeId, std::string> incLabels = m_Labels;
    std::unordered_map<uint32_t, TreeId> incParentKey = m_EntityParentKey;

    Rebuild(); // authoritative; overwrites members

    const bool ok = (incRoots == m_Roots) && (incChildren == m_Children) && (incLabels == m_Labels);
    if (!ok)
    {
        Logger::Log::Error("[Hierarchy] incremental sync diverged from a fresh Rebuild");
        assert(false && "HierarchyDataProvider incremental sync diverged from Rebuild");
    }

    m_Roots = std::move(incRoots);
    m_Children = std::move(incChildren);
    m_Labels = std::move(incLabels);
    m_EntityParentKey = std::move(incParentKey);
}
#endif

void HierarchyDataProvider::SetGhostRows(std::vector<GhostRow> rows)
{
    if (rows.empty() && m_GhostChildren.empty())
        return;
    m_GhostChildren.clear();
    m_GhostLabels.clear();
    for (GhostRow& row : rows)
    {
        m_GhostChildren[row.ParentId].push_back(row.Id);
        m_GhostLabels.emplace(row.Id, std::move(row.Label));
    }
    MarkStructureChanged();
    MarkAllChanged();
}

int HierarchyDataProvider::GhostChildCount(TreeId parent) const
{
    const auto it = m_GhostChildren.find(parent);
    return it == m_GhostChildren.end() ? 0 : static_cast<int>(it->second.size());
}

std::span<const HierarchyDataProvider::EntityHandle> HierarchyDataProvider::ChildrenOf(const EntityHandle& entity) const
{
    const auto it = m_Children.find(static_cast<uint32_t>(Encode(entity)));
    if (it == m_Children.end())
        return {};
    return it->second;
}

int HierarchyDataProvider::GetChildCount(TreeId parent) const
{
    if (parent == kRootId)
        return static_cast<int>(m_Roots.size()) + GhostChildCount(parent);
    if (Editor::IsGhostTreeId(parent))
        return GhostChildCount(parent);
    const auto pid = static_cast<uint32_t>(parent);
    auto it = m_Children.find(pid);
    const int entityCount = it == m_Children.end() ? 0 : static_cast<int>(it->second.size());
    return entityCount + GhostChildCount(parent);
}

TreeId HierarchyDataProvider::GetChildId(TreeId parent, int index) const
{
    // Ghost rows sort after the live children of the same parent: a deleted
    // sibling should not displace the entities that are still there.
    const auto ghostAt = [this, parent](int ghostIndex) -> TreeId
    {
        const auto it = m_GhostChildren.find(parent);
        if (it == m_GhostChildren.end() || ghostIndex < 0 ||
            ghostIndex >= static_cast<int>(it->second.size()))
            return 0;
        return it->second[static_cast<size_t>(ghostIndex)];
    };

    if (parent == kRootId)
    {
        const int rootCount = static_cast<int>(m_Roots.size());
        if (index >= 0 && index < rootCount)
            return Encode(m_Roots[static_cast<size_t>(index)]);
        return ghostAt(index - rootCount);
    }
    if (Editor::IsGhostTreeId(parent))
        return ghostAt(index);
    const auto pid = static_cast<uint32_t>(parent);
    auto it = m_Children.find(pid);
    const int entityCount = it == m_Children.end() ? 0 : static_cast<int>(it->second.size());
    if (index >= 0 && index < entityCount)
        return Encode(it->second[static_cast<size_t>(index)]);
    return ghostAt(index - entityCount);
}

const char* HierarchyDataProvider::GetLabel(TreeId id) const
{
    if (Editor::IsGhostTreeId(id))
    {
        const auto ghost = m_GhostLabels.find(id);
        return ghost != m_GhostLabels.end() ? ghost->second.c_str() : "";
    }
    auto it = m_Labels.find(id);
    if (it != m_Labels.end())
        return it->second.c_str();
    return "";
}

bool HierarchyDataProvider::IsExpandable(TreeId id) const
{
    if (id == kRootId)
        return true;
    if (Editor::IsGhostTreeId(id))
        return GhostChildCount(id) > 0;
    const auto pid = static_cast<uint32_t>(id);
    auto it = m_Children.find(pid);
    if (it != m_Children.end() && !it->second.empty())
        return true;
    return GhostChildCount(id) > 0;
}

TreeId HierarchyDataProvider::Encode(const EntityHandle& e)
{
    // Pack [version (upper kEntityVersionBits) | index (lower kEntityIndexBits)] explicitly.
    // This avoids relying on EntityHandle's union layout when round-tripping through TreeId.
    uint32 idx = static_cast<uint32>(e.index) & ECS::kEntityIndexMask;
    uint32 ver = static_cast<uint32>(e.version) & ECS::kEntityVersionMask;
    uint32 raw = idx | (ver << ECS::kEntityIndexBits);
    return static_cast<TreeId>(raw);
}

HierarchyDataProvider::EntityHandle HierarchyDataProvider::Decode(TreeId id)
{
    uint32 raw = static_cast<uint32>(id);
    ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
    ECS::EntityVersion ver = static_cast<ECS::EntityVersion>((raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
    return EntityHandle(idx, ver);
}

std::int32_t HierarchyDataProvider::OrderOf(const EntityHandle& h) const
{
    if (auto* o = m_World->GetComponent<Components::HierarchyOrder>(h))
        return o->order;
    return 0;
}

std::string HierarchyDataProvider::LowerNameOf(const EntityHandle& h) const
{
    std::string name;
    if (auto* n = m_World->GetComponent<Components::Name>(h); n && n->value[0])
        name = n->View();
    else
        name = std::string("Entity ") + std::to_string(h.id);
    name = ToLowerAscii(name);
    return name;
}

int HierarchyDataProvider::TypeRankOf(const EntityHandle& h) const
{
    // Lower rank sorts first in ascending order. Groups roughly by how "structural" the
    // entity is in the scene.
    if (m_World->GetComponent<Components::Camera>(h))
        return 0;
    if (m_World->GetComponent<Components::Light>(h))
        return 1;
    if (m_World->GetComponent<Components::Skybox>(h))
        return 2;
    if (m_World->GetComponent<Components::Terrain>(h))
        return 3;
    if (m_World->GetComponent<Components::ParticleEmitter3D>(h))
        return 4;
    if (m_World->GetComponent<Components::MeshRenderer>(h))
        return 5;
    return 6; // Empty / other
}

HierarchyDataProvider::SortKey HierarchyDataProvider::KeyOf(const EntityHandle& e) const
{
    SortKey k;
    k.Id = e.id;
    switch (m_SortMode)
    {
    case HierarchySortMode::Alphabetical:
        k.NameKey = LowerNameOf(e);
        break;
    case HierarchySortMode::Type:
        k.TypeRank = TypeRankOf(e);
        k.NameKey = LowerNameOf(e);
        break;
    case HierarchySortMode::Custom:
    default:
        k.Order = OrderOf(e);
        break;
    }
    return k;
}

bool HierarchyDataProvider::KeyLess(const SortKey& a, const SortKey& b) const
{
    return (m_SortDirection == HierarchySortDirection::Descending) ? KeyLessAsc(b, a) : KeyLessAsc(a, b);
}

bool HierarchyDataProvider::KeyLessAsc(const SortKey& a, const SortKey& b) const
{
    switch (m_SortMode)
    {
    case HierarchySortMode::Alphabetical:
        if (a.NameKey != b.NameKey)
            return a.NameKey < b.NameKey;
        return a.Id < b.Id;
    case HierarchySortMode::Type:
        if (a.TypeRank != b.TypeRank)
            return a.TypeRank < b.TypeRank;
        if (a.NameKey != b.NameKey)
            return a.NameKey < b.NameKey;
        return a.Id < b.Id;
    case HierarchySortMode::Custom:
    default:
        if (a.Order != b.Order)
            return a.Order < b.Order;
        return a.Id < b.Id;
    }
}

void HierarchyDataProvider::SortGroup(std::vector<EntityHandle>& group)
{
    if (group.size() < 2)
        return;
    m_SortScratch.clear();
    m_SortScratch.reserve(group.size());
    for (const EntityHandle& e : group)
        m_SortScratch.push_back(SortScratchEntry{e, KeyOf(e)});
    std::sort(m_SortScratch.begin(), m_SortScratch.end(),
              [this](const SortScratchEntry& a, const SortScratchEntry& b)
              { return KeyLess(a.Key, b.Key); });
    for (size_t i = 0; i < group.size(); ++i)
        group[i] = m_SortScratch[i].Handle;
}

void HierarchyDataProvider::SortedInsert(std::vector<EntityHandle>& list, const EntityHandle& e)
{
    const SortKey key = KeyOf(e);
    auto pos = std::lower_bound(list.begin(), list.end(), key,
                                [this](const EntityHandle& a, const SortKey& k)
                                { return KeyLess(KeyOf(a), k); });
    list.insert(pos, e);
}

void HierarchyDataProvider::RemoveFromSiblingList(TreeId storedParent, const EntityHandle& e)
{
    if (storedParent == kRootId)
    {
        auto it = std::find(m_Roots.begin(), m_Roots.end(), e);
        if (it != m_Roots.end())
            m_Roots.erase(it);
        return;
    }
    auto cit = m_Children.find(static_cast<uint32_t>(storedParent));
    if (cit == m_Children.end())
        return;
    auto& list = cit->second;
    auto it = std::find(list.begin(), list.end(), e);
    if (it != list.end())
        list.erase(it);
    if (list.empty())
        m_Children.erase(cit);
}

TreeId HierarchyDataProvider::DesiredParentKey(const EntityHandle& e) const
{
    if (auto* p = m_World->GetComponent<Components::Parent>(e); p && p->Entity.IsValid())
    {
        const uint32_t pk = static_cast<uint32_t>(Encode(p->Entity));
        if (m_EntityParentKey.find(pk) != m_EntityParentKey.end())
            return static_cast<TreeId>(pk);
    }
    return kRootId;
}

void HierarchyDataProvider::ComputeLabel(const EntityHandle& e, std::string& out) const
{
    if (auto* name = m_World->GetComponent<Components::Name>(e); name && name->value[0])
        out.assign(name->View());
    else
    {
        out.assign("Entity ");
        out += std::to_string(e.id);
    }
}

void HierarchyDataProvider::SeedParentKeyIndex()
{
    m_EntityParentKey.clear();
    // Reserve for the total tracked entity count (roots + all children == the hide-filtered
    // snapshot), not m_Children.size() which is the parent-bucket count.
    m_EntityParentKey.reserve(m_SnapshotScratch.size());
    for (const EntityHandle& r : m_Roots)
        m_EntityParentKey[static_cast<uint32_t>(Encode(r))] = kRootId;
    for (const auto& kv : m_Children)
        for (const EntityHandle& c : kv.second)
            m_EntityParentKey[static_cast<uint32_t>(Encode(c))] = static_cast<TreeId>(kv.first);
}

} // namespace GameEngine
