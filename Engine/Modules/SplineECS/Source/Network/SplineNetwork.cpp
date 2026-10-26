#include "SplineECS/SplineNetwork.h"

#include "Network/SplineNetworkDetect.h"
#include "SplineECS/SplineService.h"

#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "Mathematics/Geometry.h"
#include "Spline/SplineData.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string_view>
#include <utility>

namespace GameEngine::SplineECS
{
namespace
{

using Components::SplineNetworkMemberKind;
using Components::SplineNetworkOverride;
using Components::SplineNetworkOverrides;
using Components::SplineNodeRule;
using Mathematics::GroundDistance;
using Mathematics::Vector3;
using Network::MemberCurve;
using Network::RawNode;

// The node an Attach record made, by its holder, its slot and the node's index.
struct AttachNode
{
    ECS::EntityHandle Holder{};
    uint32 Slot = 0;
    size_t NodeIndex = 0;
};

// A node id and the node's index in the node list, sorted by id.
using NodeIndex = std::pair<uint32, uint32>;

// Two members' lengths closer than this are equal for choosing the ordering member, in meters.
constexpr float32 kOrderingLengthResolutionMetres = 0.01f;
// Every node of a pair moved by one vector, to this tolerance in meters.
constexpr float32 kOneVectorToleranceMetres = 0.01f;

uint64 PairKey(ECS::EntityHandle a, ECS::EntityHandle b)
{
    const uint32 low = std::min(a.id, b.id);
    const uint32 high = std::max(a.id, b.id);
    return (static_cast<uint64>(low) << 32) | high;
}

uint64 PairKeyOf(const SplineNode& node)
{
    return PairKey(node.Members[0], node.Members[1]);
}

// The distance between two arc lengths along `curve`, modulo the loop on a closed curve.
float32 ArcDistance(const MemberCurve& curve, float32 a, float32 b)
{
    const float32 apart = std::fabs(curve.Wrap(a) - curve.Wrap(b));
    return curve.Closed ? std::min(apart, curve.Length - apart) : apart;
}

// The node's arc length along `member`, one of its two members, in world meters.
float32 ArcAlong(const SplineNode& node, ECS::EntityHandle member)
{
    return node.Members[0] == member ? node.ArcLengthMetres[0] : node.ArcLengthMetres[1];
}

// The rank of each node along `member`, the nodes listed in order along the pair's other member.
std::vector<uint32> RanksAlong(std::span<const SplineNode> nodes, ECS::EntityHandle member)
{
    std::vector<uint32> order(nodes.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&nodes, member](uint32 left, uint32 right) {
        return ArcAlong(nodes[left], member) < ArcAlong(nodes[right], member);
    });
    std::vector<uint32> ranks(nodes.size());
    for (uint32 rank = 0; rank < order.size(); ++rank)
        ranks[order[rank]] = rank;
    return ranks;
}

std::string_view SceneId(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(entity);
    return tag ? tag->View() : std::string_view{};
}

// Two lists of Attach meetings make the same nodes: the same kinds, members, arc lengths and slots.
bool SameAttaches(const std::vector<RawNode>& a, const std::vector<RawNode>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].Kind != b[i].Kind || a[i].Members[0] != b[i].Members[0] || a[i].Members[1] != b[i].Members[1] ||
            !(a[i].ArcLengths == b[i].ArcLengths) || a[i].AttachSlot != b[i].AttachSlot)
            return false;
    return true;
}

// The index of the one position within kNodeMatchMetres of `position` that is clearly the
// nearest (the next one is farther by more than kNodeTouchMetres), or -1.
int32 UniqueNearest(std::span<const Vector3> positions, const Vector3& position)
{
    int32 nearest = -1;
    float32 nearestDistance = kNodeMatchMetres;
    float32 secondDistance = std::numeric_limits<float32>::max();
    for (size_t i = 0; i < positions.size(); ++i)
    {
        const float32 distance = GroundDistance(positions[i], position);
        if (distance <= nearestDistance)
        {
            secondDistance = nearest >= 0 ? nearestDistance : secondDistance;
            nearestDistance = distance;
            nearest = static_cast<int32>(i);
        }
        else
        {
            secondDistance = std::min(secondDistance, distance);
        }
    }
    return nearest >= 0 && secondDistance - nearestDistance > kNodeTouchMetres ? nearest : -1;
}

// Rule (2) by position: every node and every previous record are each other's unique nearest
// within reach. Gives an id per node, or nothing when any one is not.
std::vector<uint32> MatchMutualNearest(std::span<const Vector3> previous, std::span<const uint32> previousIds,
                                       std::span<const Vector3> positions)
{
    if (previous.size() != positions.size())
        return {};
    std::vector<uint32> ids(positions.size(), 0);
    for (size_t i = 0; i < positions.size(); ++i)
    {
        const int32 record = UniqueNearest(previous, positions[i]);
        if (record < 0 || UniqueNearest(positions, previous[record]) != static_cast<int32>(i))
            return {};
        ids[i] = previousIds[record];
    }
    return ids;
}

// Rule (3): a node takes the one previous record within reach when that record reaches no other
// node. A node with no such record is new.
std::vector<uint32> MatchUniqueInReach(std::span<const Vector3> previous, std::span<const uint32> previousIds,
                                       std::span<const Vector3> positions)
{
    std::vector<uint32> ids(positions.size(), 0);
    for (size_t i = 0; i < positions.size(); ++i)
    {
        size_t nearRecords = 0;
        size_t match = 0;
        for (size_t record = 0; record < previous.size(); ++record)
            if (GroundDistance(positions[i], previous[record]) <= kNodeMatchMetres)
            {
                ++nearRecords;
                match = record;
            }
        if (nearRecords != 1)
            continue;
        size_t nearNodes = 0;
        for (const Vector3& other : positions)
            nearNodes += GroundDistance(other, previous[match]) <= kNodeMatchMetres ? 1 : 0;
        if (nearNodes == 1)
            ids[i] = previousIds[match];
    }
    return ids;
}

// The nodes sorted by id, with their indices.
std::vector<NodeIndex> SortedById(const std::vector<SplineNode>& nodes)
{
    std::vector<NodeIndex> sorted;
    sorted.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        sorted.emplace_back(nodes[i].Id, static_cast<uint32>(i));
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

// Nodes are kept grouped by pair in ascending pair key order: the comparisons that find a pair's run.
bool NodeKeyBelow(const SplineNode& node, uint64 key)
{
    return PairKeyOf(node) < key;
}

bool KeyBelowNode(uint64 key, const SplineNode& node)
{
    return key < PairKeyOf(node);
}

// Whether the node changed in a way a consumer reads.
bool NodeChanged(const SplineNode& was, const SplineNode& now)
{
    return GroundDistance(was.Position, now.Position) > kNodeTouchMetres || was.Kind != now.Kind ||
           was.Rule != now.Rule || was.IsSplitLevel() != now.IsSplitLevel() || was.ArmCount != now.ArmCount;
}

} // namespace

bool SplineNode::IsSplitLevel() const
{
    if (Rule == SplineNodeRule::SplitLevel)
        return true;
    return Rule != SplineNodeRule::AtGrade && std::fabs(HeightDifferenceMetres) > kSplitLevelToleranceMetres;
}

// One world's network.
struct SplineNetwork::State
{
    struct CachedCurve
    {
        uint64 SplineVersion = 0;
        float32 SplineLength = 0.0f;
        Mathematics::Matrix4x4 Matrix{};
        // Bumped at every resample; DerivedGeneration is the one the last derivation read.
        uint64 Generation = 1;
        uint64 DerivedGeneration = 0;
        bool Seen = false;
        MemberCurve Curve;
    };

    struct PairState
    {
        ECS::EntityHandle Ordering{};
        // The pair's nodes in order along the ordering member, where each was last derived and
        // each one's rank along the other member.
        std::vector<uint32> NodeIds;
        std::vector<Vector3> Positions;
        std::vector<uint32> OtherRanks;
        // The first sample of the member with the lower id, then the other's, at that
        // derivation, for the rigid-move test.
        Vector3 FirstStart{};
        Vector3 SecondStart{};
    };

    struct Binding
    {
        uint32 NodeId = 0;
        Vector3 LastPosition{};
        bool HasPosition = false;
    };

    struct Holder
    {
        SplineNetworkOverrides Written{};
        Binding Bindings[Components::kMaxSplineNetworkOverrides] = {};
    };

    // A record of this derivation and the node it may take.
    struct Claim
    {
        ECS::EntityHandle Holder{};
        uint32 Slot = 0;
        uint32 HolderCurve = 0;
        // Bound to its candidate at the last derivation (or made it, for an Attach record).
        bool Follows = false;
        // Matched by its saved arc lengths: a write the service did not make.
        bool Loaded = false;
        std::vector<uint32> Candidates;
    };

    // The nodes of one pair as built at the two members' generations, members by entity id. A
    // pair whose generations and Attach meetings are unchanged keeps its nodes as they are.
    struct PairCache
    {
        uint64 Key = 0;
        uint64 Generations[2] = {};
        // The meetings the geometry makes, and the Attach meetings added to them.
        std::vector<RawNode> Found;
        std::vector<RawNode> Attaches;
        // Built and identified; RuleHolder and RuleSlot name the Attach record that made a node.
        std::vector<SplineNode> Nodes;
    };

    uint64 ResetGeneration = 0;
    FlatMap<uint32, CachedCurve> Curves;
    // The curves the network reads (every member but an open Area), in entity-id order, and each
    // one's index by entity id. Rebuilt when a curve is added, removed or resampled.
    std::vector<const MemberCurve*> Members;
    FlatMap<uint32, uint32> MemberByEntity;
    // Sorted by Key: every pair with a node at the last derivation.
    std::vector<PairCache> Cache;
    FlatMap<uint64, PairState> Pairs;
    FlatMap<uint32, Holder> Holders;
    // Grouped by pair in ascending pair key order.
    std::vector<SplineNode> Nodes;
    std::vector<NodeIndex> NodeById;
    std::vector<SplineHeldRecord> Held;
    std::vector<SplineMemberIssue> Issues;
    bool Dirty = true;

    bool RefreshCurves(ECS::World& world);
    bool RefreshCurve(const SplineService& service, ECS::EntityHandle entity,
                      const Components::SplineNetworkMember& member, const Components::SplineComponent& component,
                      const Components::WorldTransform& transform);
    void RebuildMembers();
    bool OverridesChanged(const ECS::World& world) const;
    std::vector<SplineNodeChange> Derive(ECS::World& world, uint32& nextNodeId);
    std::vector<std::pair<uint64, RawNode>> AttachMeetings(const ECS::World& world) const;
    ECS::EntityHandle ChooseOrdering(const ECS::World& world, const MemberCurve& a, const MemberCurve& b) const;
    uint64 LastGeneration(uint32 entityId) const;
    void BuildPair(const ECS::World& world, const MemberCurve& first, const MemberCurve& second,
                   const std::vector<RawNode>& raw, std::vector<SplineNode>& built, uint32& nextNodeId);
    void MatchPairNodes(const ECS::World& world, const MemberCurve& first, const MemberCurve& second,
                        std::vector<SplineNode>& pairNodes, uint32& nextNodeId);
    void AppendPairNodes(const std::vector<SplineNode>& built, std::vector<AttachNode>& attachNodes);
    void MatchRecords(ECS::World& world, const std::vector<AttachNode>& attachNodes);
    std::vector<SplineNodeChange> Changes(const std::vector<SplineNode>& previous) const;
    std::span<const SplineNode> NodesOfPair(uint64 key) const;
    PairCache* FindCache(uint64 key);
    // The node's index in Nodes, or Nodes.size() when no node has the id.
    size_t IndexOf(uint32 id) const;
    const SplineNode* FindNode(uint32 id) const;
    RawNode ToIndices(RawNode node) const;
    RawNode ToEntityIds(RawNode node) const;
};

std::unique_ptr<SplineNetwork> SplineNetwork::s_Instance;

void SplineNetwork::Initialize()
{
    assert(!s_Instance && "SplineNetwork already initialized");
    s_Instance = std::make_unique<SplineNetwork>();
}

void SplineNetwork::Shutdown()
{
    s_Instance.reset();
}

SplineNetwork& SplineNetwork::Get()
{
    assert(s_Instance && "SplineNetwork not initialized");
    return *s_Instance;
}

SplineNetwork* SplineNetwork::TryGet()
{
    return s_Instance.get();
}

bool SplineNetwork::IsInitialized()
{
    return s_Instance != nullptr;
}

SplineNetwork::SplineNetwork() = default;

SplineNetwork::~SplineNetwork() = default;

bool SplineNetwork::State::RefreshCurve(const SplineService& service, ECS::EntityHandle entity,
                                        const Components::SplineNetworkMember& member,
                                        const Components::SplineComponent& component,
                                        const Components::WorldTransform& transform)
{
    const Spline::SplineData* data =
        service.GetSplineData(SplineHandle(component.SplineDataIndex, component.SplineDataGeneration));
    if (!data || !data->IsValid() || data->Dirty || data->ArcLengthTable.empty())
        return false;
    CachedCurve& cached = Curves.GetOrInsert(entity.id);
    cached.Seen = true;
    cached.Curve.Spline = data;
    const bool same = cached.Curve.Entity == entity && cached.Curve.Kind == member.Kind &&
                      cached.SplineVersion == data->Version && cached.SplineLength == data->TotalArcLength &&
                      std::memcmp(cached.Matrix.Data(), transform.matrix, sizeof(transform.matrix)) == 0;
    if (same)
        return false;
    cached.SplineVersion = data->Version;
    cached.SplineLength = data->TotalArcLength;
    cached.Matrix = Mathematics::Matrix4x4::FromColumnMajor(transform.matrix);
    ++cached.Generation;
    cached.Curve = Network::SampleMemberCurve(entity, member.Kind, *data, cached.Matrix);
    return true;
}

// Every member is read on every update, since a transform edit moves a member without
// touching its spline; only a member whose spline or transform changed is resampled.
bool SplineNetwork::State::RefreshCurves(ECS::World& world)
{
    const SplineService* service = SplineService::TryGet();
    if (!service)
        return false;
    bool changed = false;
    for (auto& [id, cached] : Curves)
        cached.Seen = false;
    world.Query<ECS::Read<Components::SplineNetworkMember>, ECS::Read<Components::SplineComponent>,
                ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, const Components::SplineNetworkMember& member,
                  const Components::SplineComponent& component, const Components::WorldTransform& transform) {
            changed = RefreshCurve(*service, entity, member, component, transform) || changed;
        });
    changed = Curves.EraseIf([](uint32, const CachedCurve& cached) { return !cached.Seen; }) > 0 || changed;
    if (changed)
        RebuildMembers();
    return changed;
}

void SplineNetwork::State::RebuildMembers()
{
    Members.clear();
    MemberByEntity.Clear();
    Issues.clear();
    for (const auto& [id, cached] : Curves)
    {
        if (cached.Curve.Kind == SplineNetworkMemberKind::Area && !cached.Curve.Closed)
        {
            Issues.push_back({cached.Curve.Entity, SplineMemberIssueKind::OpenArea});
            continue;
        }
        MemberByEntity.InsertOrAssign(id, static_cast<uint32>(Members.size()));
        Members.push_back(&cached.Curve);
    }
}

bool SplineNetwork::State::OverridesChanged(const ECS::World& world) const
{
    for (const MemberCurve* curve : Members)
    {
        const auto* overrides = world.GetComponent<SplineNetworkOverrides>(curve->Entity);
        const Holder* holder = Holders.Find(curve->Entity.id);
        if (!overrides ? holder != nullptr
                       : !holder || std::memcmp(&holder->Written, overrides, sizeof(*overrides)) != 0)
            return true;
    }
    return false;
}

ECS::EntityHandle SplineNetwork::State::ChooseOrdering(const ECS::World& world, const MemberCurve& a,
                                                       const MemberCurve& b) const
{
    if (std::fabs(a.Length - b.Length) > kOrderingLengthResolutionMetres)
        return a.Length > b.Length ? a.Entity : b.Entity;
    const Vector3& startA = a.Points.front();
    const Vector3& startB = b.Points.front();
    if (std::fabs(startA.x - startB.x) > kNodeTouchMetres)
        return startA.x < startB.x ? a.Entity : b.Entity;
    if (std::fabs(startA.z - startB.z) > kNodeTouchMetres)
        return startA.z < startB.z ? a.Entity : b.Entity;
    const std::string_view idA = SceneId(world, a.Entity);
    const std::string_view idB = SceneId(world, b.Entity);
    if (!idA.empty() && !idB.empty() && idA != idB)
        return idA < idB ? a.Entity : b.Entity;
    // Never saved and tied on every key: the first-derived order, which is the derivation's
    // entity order, kept until the next load.
    return a.Entity;
}

// Rule (2) in order: the count is unchanged, the nodes keep their order along both members, and
// every node lies within reach of its in-order record or the pair moved rigidly (every node and
// both members' starts moved by one vector). A reversed member or a moved loop start changes the
// order along that member only, so it never passes. Otherwise the match is by position: mutual
// and unique when the count is unchanged, unique within reach when it changed (rule 3).
void SplineNetwork::State::MatchPairNodes(const ECS::World& world, const MemberCurve& first, const MemberCurve& second,
                                          std::vector<SplineNode>& pairNodes, uint32& nextNodeId)
{
    PairState& pair = Pairs.GetOrInsert(PairKey(first.Entity, second.Entity));
    if (pair.NodeIds.empty())
        pair.Ordering = ChooseOrdering(world, first, second);
    const ECS::EntityHandle ordering = pair.Ordering;
    std::sort(pairNodes.begin(), pairNodes.end(), [ordering](const SplineNode& left, const SplineNode& right) {
        return ArcAlong(left, ordering) < ArcAlong(right, ordering);
    });

    const MemberCurve& low = first.Entity.id < second.Entity.id ? first : second;
    const MemberCurve& high = first.Entity.id < second.Entity.id ? second : first;
    const size_t count = pairNodes.size();
    std::vector<Vector3> positions;
    positions.reserve(count);
    for (const SplineNode& node : pairNodes)
        positions.push_back(node.Position);

    const ECS::EntityHandle other = ordering == first.Entity ? second.Entity : first.Entity;
    std::vector<uint32> ranks = RanksAlong(pairNodes, other);
    std::vector<uint32> ids;
    if (count == pair.NodeIds.size() && ranks == pair.OtherRanks)
    {
        const Vector3 shift = positions[0] - pair.Positions[0];
        bool inReach = true;
        bool oneVector = true;
        for (size_t i = 0; i < count; ++i)
        {
            inReach = inReach && GroundDistance(positions[i], pair.Positions[i]) <= kNodeMatchMetres;
            oneVector =
                oneVector && GroundDistance(positions[i] - pair.Positions[i], shift) <= kOneVectorToleranceMetres;
        }
        const bool rigid = oneVector &&
                           GroundDistance(low.Points.front() - pair.FirstStart, shift) <= kOneVectorToleranceMetres &&
                           GroundDistance(high.Points.front() - pair.SecondStart, shift) <= kOneVectorToleranceMetres;
        if (inReach || rigid)
            ids = pair.NodeIds;
    }
    if (ids.empty() && count == pair.NodeIds.size())
        ids = MatchMutualNearest(pair.Positions, pair.NodeIds, positions);
    if (ids.empty())
        ids = MatchUniqueInReach(pair.Positions, pair.NodeIds, positions);

    pair.NodeIds.clear();
    pair.Positions.clear();
    pair.OtherRanks = std::move(ranks);
    pair.FirstStart = low.Points.front();
    pair.SecondStart = high.Points.front();
    for (size_t i = 0; i < count; ++i)
    {
        SplineNode& node = pairNodes[i];
        node.Id = ids[i] != 0 ? ids[i] : nextNodeId++;
        pair.NodeIds.push_back(node.Id);
        pair.Positions.push_back(node.Position);
    }
}

void SplineNetwork::State::MatchRecords(ECS::World& world, const std::vector<AttachNode>& attachNodes)
{
    Held.clear();
    std::vector<Claim> claims;
    for (const MemberCurve* curve : Members)
    {
        const auto* overrides = world.GetComponent<SplineNetworkOverrides>(curve->Entity);
        if (!overrides)
        {
            Holders.Erase(curve->Entity.id);
            continue;
        }
        Holder& holder = Holders.GetOrInsert(curve->Entity.id);
        // Every write that is not the service's own is a load for this entity's records.
        if (std::memcmp(&holder.Written, overrides, sizeof(*overrides)) != 0)
            for (Binding& binding : holder.Bindings)
                binding = {};
        const uint32 holderCurve = *MemberByEntity.Find(curve->Entity.id);
        for (uint32 slot = 0; slot < Components::kMaxSplineNetworkOverrides; ++slot)
        {
            const SplineNetworkOverride& record = overrides->Records[slot];
            Binding& binding = holder.Bindings[slot];
            if (record.Rule == SplineNodeRule::None)
            {
                binding = {};
                continue;
            }
            Claim claim;
            claim.Holder = curve->Entity;
            claim.Slot = slot;
            claim.HolderCurve = holderCurve;
            const uint32* otherCurve = MemberByEntity.Find(record.Other.id);
            if (!otherCurve || record.Other == curve->Entity)
            {
                claims.push_back(std::move(claim));
                continue;
            }
            const MemberCurve& other = *Members[*otherCurve];
            if (record.Rule == SplineNodeRule::Attach)
            {
                for (const AttachNode& made : attachNodes)
                    if (made.Holder == curve->Entity && made.Slot == slot)
                    {
                        claim.Candidates.push_back(Nodes[made.NodeIndex].Id);
                        claim.Follows = true;
                    }
                claims.push_back(std::move(claim));
                continue;
            }
            const uint64 key = PairKey(curve->Entity, record.Other);
            if (const SplineNode* bound = binding.NodeId != 0 ? FindNode(binding.NodeId) : nullptr;
                bound && PairKeyOf(*bound) == key)
            {
                claim.Candidates.push_back(bound->Id);
                claim.Follows = true;
                claims.push_back(std::move(claim));
                continue;
            }
            claim.Loaded = !binding.HasPosition;
            for (const SplineNode& node : NodesOfPair(key))
            {
                bool within = false;
                if (binding.HasPosition)
                {
                    within = GroundDistance(node.Position, binding.LastPosition) <= kNodeMatchMetres;
                }
                else
                {
                    const uint32 self = node.Members[0] == curve->Entity ? 0 : 1;
                    within = ArcDistance(*curve, record.ArcLengthMetres, node.ArcLengthMetres[self]) <= kNodeMatchMetres &&
                             ArcDistance(other, record.OtherArcLengthMetres, node.ArcLengthMetres[1 - self]) <=
                                 kNodeMatchMetres;
                }
                if (within)
                    claim.Candidates.push_back(node.Id);
            }
            claims.push_back(std::move(claim));
        }
    }

    // A node takes a record when exactly one record reaches it, or when one record follows it
    // and no loaded record reaches it. A loaded record that reaches a followed node is a second
    // record for that node (a script's write, a hand-edited file): both are held, each with the
    // node as its candidate, until the author keeps one.
    FlatMap<uint32, uint32> reachCount;
    FlatMap<uint32, uint32> followCount;
    FlatMap<uint32, uint32> loadCount;
    for (const Claim& claim : claims)
        for (const uint32 id : claim.Candidates)
        {
            ++reachCount.GetOrInsert(id);
            if (claim.Follows)
                ++followCount.GetOrInsert(id);
            if (claim.Loaded)
                ++loadCount.GetOrInsert(id);
        }

    FlatMap<uint32, SplineNetworkOverrides> written;
    for (const Claim& claim : claims)
    {
        if (!written.Find(claim.Holder.id))
            written.InsertOrAssign(claim.Holder.id, *world.GetComponent<SplineNetworkOverrides>(claim.Holder));
        SplineNetworkOverride& record = written.Find(claim.Holder.id)->Records[claim.Slot];
        Binding& binding = Holders.Find(claim.Holder.id)->Bindings[claim.Slot];
        bool takes = false;
        if (claim.Candidates.size() == 1)
        {
            const uint32 id = claim.Candidates[0];
            const uint32* follows = followCount.Find(id);
            const uint32* loads = loadCount.Find(id);
            takes = claim.Follows ? *follows == 1 && !loads : *reachCount.Find(id) == 1;
        }
        if (takes)
        {
            SplineNode& node = Nodes[IndexOf(claim.Candidates[0])];
            const uint32 self = node.Members[0] == claim.Holder ? 0 : 1;
            node.Rule = record.Rule;
            node.RuleHolder = claim.Holder;
            node.RuleSlot = claim.Slot;
            record.ArcLengthMetres = node.ArcLengthMetres[self];
            record.OtherArcLengthMetres = node.ArcLengthMetres[1 - self];
            binding = {node.Id, node.Position, true};
            continue;
        }
        if (!binding.HasPosition)
            binding.LastPosition = Members[claim.HolderCurve]->PointAt(record.ArcLengthMetres);
        binding.HasPosition = true;
        binding.NodeId = 0;
        SplineHeldRecord held;
        held.Holder = claim.Holder;
        held.Slot = claim.Slot;
        held.Record = record;
        held.LastPosition = binding.LastPosition;
        held.CandidateNodes = claim.Candidates;
        Held.push_back(std::move(held));
    }

    for (auto& [id, overrides] : written)
    {
        const ECS::EntityHandle entity(id);
        if (auto* current = world.GetComponentForWrite<SplineNetworkOverrides>(entity))
            if (std::memcmp(current, &overrides, sizeof(overrides)) != 0)
                *current = overrides;
        Holders.Find(id)->Written = overrides;
    }
    for (const MemberCurve* curve : Members)
        if (Holder* holder = Holders.Find(curve->Entity.id); holder && !written.Find(curve->Entity.id))
            holder->Written = *world.GetComponent<SplineNetworkOverrides>(curve->Entity);
}

// The meetings the Attach records make, by pair key, members by index. Each sits at the holder's
// end nearest the record's arc length, on the other member at the record's other arc length;
// when the other member changed since the record was bound, the attach point stays where it was
// and the arc length is taken again from it, so an upstream edit of the body does not slide it.
std::vector<std::pair<uint64, RawNode>> SplineNetwork::State::AttachMeetings(const ECS::World& world) const
{
    std::vector<std::pair<uint64, RawNode>> attaches;
    for (uint32 holderIndex = 0; holderIndex < Members.size(); ++holderIndex)
    {
        const MemberCurve& holder = *Members[holderIndex];
        const auto* overrides = world.GetComponent<SplineNetworkOverrides>(holder.Entity);
        if (!overrides || holder.Closed || holder.Kind != SplineNetworkMemberKind::Road)
            continue;
        const Holder* known = Holders.Find(holder.Entity.id);
        const bool loaded = !known || std::memcmp(&known->Written, overrides, sizeof(*overrides)) != 0;
        for (uint32 slot = 0; slot < Components::kMaxSplineNetworkOverrides; ++slot)
        {
            const SplineNetworkOverride& record = overrides->Records[slot];
            const uint32* otherIndex = MemberByEntity.Find(record.Other.id);
            if (record.Rule != SplineNodeRule::Attach || !otherIndex || *otherIndex == holderIndex)
                continue;
            const MemberCurve& other = *Members[*otherIndex];
            float32 otherArc = record.OtherArcLengthMetres;
            const bool otherMoved = Curves.Find(other.Entity.id)->Generation != LastGeneration(other.Entity.id);
            if (!loaded && otherMoved && known->Bindings[slot].HasPosition)
                otherArc = Network::NearestPointXZ(other, known->Bindings[slot].LastPosition).ArcLength;
            RawNode node;
            node.Kind = other.Kind == SplineNetworkMemberKind::Area ? SplineNodeKind::Entry : SplineNodeKind::Attachment;
            node.Members[0] = holderIndex;
            node.Members[1] = *otherIndex;
            node.ArcLengths[0] = record.ArcLengthMetres < holder.Length * 0.5f ? 0.0f : holder.Length;
            node.ArcLengths[1] = other.Wrap(otherArc);
            node.Position = other.PointAt(node.ArcLengths[1]);
            node.AttachSlot = static_cast<int32>(slot);
            attaches.emplace_back(PairKey(holder.Entity, other.Entity), node);
        }
    }
    std::stable_sort(attaches.begin(), attaches.end(),
                     [](const auto& left, const auto& right) { return left.first < right.first; });
    return attaches;
}

RawNode SplineNetwork::State::ToIndices(RawNode node) const
{
    node.Members[0] = *MemberByEntity.Find(node.Members[0]);
    node.Members[1] = *MemberByEntity.Find(node.Members[1]);
    return node;
}

RawNode SplineNetwork::State::ToEntityIds(RawNode node) const
{
    node.Members[0] = Members[node.Members[0]]->Entity.id;
    node.Members[1] = Members[node.Members[1]]->Entity.id;
    return node;
}

std::vector<SplineNodeChange> SplineNetwork::State::Derive(ECS::World& world, uint32& nextNodeId)
{
    const std::vector<std::pair<uint64, RawNode>> attaches = AttachMeetings(world);

    // Members are in entity-id order, so pairs are visited in ascending key order, the cache is
    // rebuilt sorted and the nodes come out grouped by pair. Only a pair whose bounds overlap or
    // that an Attach record joins is visited; only a visited pair whose member was resampled is
    // detected again, and only one whose member or Attach meetings changed is built and
    // identified again.
    std::vector<Network::MemberPair> visited;
    Network::OverlappingPairs(Members, visited);
    for (const auto& attach : attaches)
    {
        const RawNode& node = attach.second;
        visited.push_back({std::min(node.Members[0], node.Members[1]), std::max(node.Members[0], node.Members[1])});
    }
    std::sort(visited.begin(), visited.end());
    visited.erase(std::unique(visited.begin(), visited.end()), visited.end());
    std::vector<PairCache> cache;
    std::vector<SplineNode> previous = std::move(Nodes);
    Nodes.clear();
    Nodes.reserve(previous.size());
    std::vector<AttachNode> attachNodes;
    std::vector<RawNode> pairAttaches;
    std::vector<RawNode> raw;
    for (const auto [i, j] : visited)
    {
        const uint64 key = PairKey(Members[i]->Entity, Members[j]->Entity);
        const uint64 generations[2] = {Curves.Find(Members[i]->Entity.id)->Generation,
                                       Curves.Find(Members[j]->Entity.id)->Generation};
        pairAttaches.clear();
        for (auto attach = std::lower_bound(attaches.begin(), attaches.end(), key,
                                            [](const auto& entry, uint64 wanted) { return entry.first < wanted; });
             attach != attaches.end() && attach->first == key; ++attach)
            pairAttaches.push_back(ToEntityIds(attach->second));
        PairCache* cached = FindCache(key);
        const bool hasCache = cached != nullptr;
        const bool sameCurves =
            hasCache && cached->Generations[0] == generations[0] && cached->Generations[1] == generations[1];
        if (sameCurves && SameAttaches(cached->Attaches, pairAttaches))
        {
            AppendPairNodes(cached->Nodes, attachNodes);
            cache.push_back(std::move(*cached));
            continue;
        }
        PairCache entry;
        entry.Key = key;
        entry.Generations[0] = generations[0];
        entry.Generations[1] = generations[1];
        raw.clear();
        if (sameCurves)
        {
            for (const RawNode& found : cached->Found)
                raw.push_back(ToIndices(found));
        }
        else if (hasCache || generations[0] != LastGeneration(Members[i]->Entity.id) ||
                 generations[1] != LastGeneration(Members[j]->Entity.id))
        {
            Network::DetectPairNodes(Members, i, j, raw);
        }
        for (const RawNode& found : raw)
            entry.Found.push_back(ToEntityIds(found));
        for (const RawNode& attach : pairAttaches)
        {
            const RawNode made = ToIndices(attach);
            // The attach replaces a meeting the geometry already makes at that end.
            std::erase_if(raw, [&made](const RawNode& found) {
                return found.Members[0] == made.Members[0] &&
                       std::fabs(found.ArcLengths[0] - made.ArcLengths[0]) <= kNodeTouchMetres;
            });
            raw.push_back(made);
        }
        entry.Attaches = pairAttaches;
        if (raw.empty())
            continue;
        BuildPair(world, *Members[i], *Members[j], raw, entry.Nodes, nextNodeId);
        AppendPairNodes(entry.Nodes, attachNodes);
        cache.push_back(std::move(entry));
    }
    Cache = std::move(cache);
    for (auto& [id, cached] : Curves)
        cached.DerivedGeneration = cached.Generation;
    Pairs.EraseIf([this](uint64 key, const PairState&) { return FindCache(key) == nullptr; });
    NodeById = SortedById(Nodes);

    MatchRecords(world, attachNodes);
    return Changes(previous);
}

uint64 SplineNetwork::State::LastGeneration(uint32 entityId) const
{
    const CachedCurve* cached = Curves.Find(entityId);
    return cached ? cached->DerivedGeneration : 0;
}

void SplineNetwork::State::BuildPair(const ECS::World& world, const MemberCurve& first, const MemberCurve& second,
                                     const std::vector<RawNode>& raw, std::vector<SplineNode>& built,
                                     uint32& nextNodeId)
{
    const MemberCurve* curves[2] = {&first, &second};
    const uint32 firstIndex = *MemberByEntity.Find(first.Entity.id);
    for (const RawNode& found : raw)
    {
        const MemberCurve& a = *curves[found.Members[0] == firstIndex ? 0 : 1];
        const MemberCurve& b = *curves[found.Members[1] == firstIndex ? 0 : 1];
        SplineNode node;
        node.Kind = found.Kind;
        node.Members[0] = a.Entity;
        node.Members[1] = b.Entity;
        node.ArcLengthMetres[0] = found.ArcLengths[0];
        node.ArcLengthMetres[1] = found.ArcLengths[1];
        node.Position = found.Position;
        Network::BuildArms(a, b, node);
        // The Attach record that made the node rides on it through the match's sort.
        if (found.AttachSlot >= 0)
        {
            node.RuleHolder = node.Members[0];
            node.RuleSlot = static_cast<uint32>(found.AttachSlot);
        }
        built.push_back(node);
    }
    MatchPairNodes(world, first, second, built, nextNodeId);
}

void SplineNetwork::State::AppendPairNodes(const std::vector<SplineNode>& built, std::vector<AttachNode>& attachNodes)
{
    for (SplineNode node : built)
    {
        if (node.RuleHolder)
            attachNodes.push_back({node.RuleHolder, node.RuleSlot, Nodes.size()});
        node.Rule = SplineNodeRule::None;
        node.RuleHolder = {};
        node.RuleSlot = 0;
        Nodes.push_back(node);
    }
}

// The nodes added, changed and removed since `previous`, by a walk of the two id-sorted lists.
std::vector<SplineNodeChange> SplineNetwork::State::Changes(const std::vector<SplineNode>& previous) const
{
    const std::vector<NodeIndex> before = SortedById(previous);
    std::vector<SplineNodeChange> changes;
    size_t was = 0;
    size_t now = 0;
    while (was < before.size() || now < NodeById.size())
    {
        if (now == NodeById.size() || (was < before.size() && before[was].first < NodeById[now].first))
        {
            changes.push_back({before[was++].first, SplineNodeChangeKind::Removed});
            continue;
        }
        if (was == before.size() || NodeById[now].first < before[was].first)
        {
            changes.push_back({NodeById[now++].first, SplineNodeChangeKind::Added});
            continue;
        }
        if (NodeChanged(previous[before[was].second], Nodes[NodeById[now].second]))
            changes.push_back({NodeById[now].first, SplineNodeChangeKind::Changed});
        ++was;
        ++now;
    }
    return changes;
}

std::span<const SplineNode> SplineNetwork::State::NodesOfPair(uint64 key) const
{
    const auto first = std::lower_bound(Nodes.begin(), Nodes.end(), key, NodeKeyBelow);
    const auto last = std::upper_bound(first, Nodes.end(), key, KeyBelowNode);
    return {first, last};
}

SplineNetwork::State::PairCache* SplineNetwork::State::FindCache(uint64 key)
{
    const auto found = std::lower_bound(Cache.begin(), Cache.end(), key,
                                        [](const PairCache& entry, uint64 wanted) { return entry.Key < wanted; });
    return found != Cache.end() && found->Key == key ? &*found : nullptr;
}

size_t SplineNetwork::State::IndexOf(uint32 id) const
{
    const auto found = std::lower_bound(NodeById.begin(), NodeById.end(), NodeIndex{id, 0});
    return found != NodeById.end() && found->first == id ? found->second : Nodes.size();
}

const SplineNode* SplineNetwork::State::FindNode(uint32 id) const
{
    const size_t index = IndexOf(id);
    return index < Nodes.size() ? &Nodes[index] : nullptr;
}

void SplineNetwork::Update(ECS::World& world)
{
    const uint64 worldId = world.GetWorldId();
    std::unique_ptr<State>* found = m_States.Find(worldId);
    if (!found)
    {
        // A world with no member costs one query and holds no state.
        if (world.Query<ECS::Read<Components::SplineNetworkMember>>().Count() == 0)
            return;
        found = &m_States.InsertOrAssign(worldId, std::make_unique<State>());
        (*found)->ResetGeneration = world.GetLifecycleResetGeneration();
    }
    State& state = **found;
    if (state.ResetGeneration != world.GetLifecycleResetGeneration())
    {
        // The world was cleared or restored from a snapshot: every handle may name another
        // entity, so every identity and binding is forgotten and every record is matched by its
        // saved keys, as at a load. Ids continue from the service's counter, so none is reused.
        state = State{};
        state.ResetGeneration = world.GetLifecycleResetGeneration();
    }
    const bool curvesChanged = state.RefreshCurves(world);
    const bool overridesChanged = state.OverridesChanged(world);
    if (!curvesChanged && !overridesChanged && !state.Dirty)
        return;
    state.Dirty = false;
    const std::vector<SplineNodeChange> changes = state.Derive(world, m_NextNodeId);
    if (state.Curves.Empty())
        m_States.Erase(worldId);
    if (!changes.empty())
        NodesChanged.Invoke(world, changes);
}

const SplineNetwork::State* SplineNetwork::FindState(const ECS::World& world) const
{
    const std::unique_ptr<State>* found = m_States.Find(world.GetWorldId());
    return found ? found->get() : nullptr;
}

std::span<const SplineNode> SplineNetwork::GetNodes(const ECS::World& world) const
{
    const State* state = FindState(world);
    return state ? std::span<const SplineNode>(state->Nodes) : std::span<const SplineNode>{};
}

const SplineNode* SplineNetwork::FindNode(const ECS::World& world, uint32 id) const
{
    const State* state = FindState(world);
    return state ? state->FindNode(id) : nullptr;
}

std::vector<const SplineNode*> SplineNetwork::FindNodes(const ECS::World& world, ECS::EntityHandle member) const
{
    std::vector<const SplineNode*> found;
    for (const SplineNode& node : GetNodes(world))
        if (node.Members[0] == member || node.Members[1] == member)
            found.push_back(&node);
    return found;
}

std::span<const SplineHeldRecord> SplineNetwork::GetHeldRecords(const ECS::World& world) const
{
    const State* state = FindState(world);
    return state ? std::span<const SplineHeldRecord>(state->Held) : std::span<const SplineHeldRecord>{};
}

std::span<const SplineMemberIssue> SplineNetwork::GetMemberIssues(const ECS::World& world) const
{
    const State* state = FindState(world);
    return state ? std::span<const SplineMemberIssue>(state->Issues) : std::span<const SplineMemberIssue>{};
}

bool SplineNetwork::KeepRecord(ECS::World& world, ECS::EntityHandle holder, uint32 slot, uint32 nodeId)
{
    std::unique_ptr<State>* found = m_States.Find(world.GetWorldId());
    if (!found)
        return false;
    State& state = **found;
    const auto held = std::find_if(state.Held.begin(), state.Held.end(), [&](const SplineHeldRecord& record) {
        return record.Holder == holder && record.Slot == slot;
    });
    if (held == state.Held.end() ||
        std::find(held->CandidateNodes.begin(), held->CandidateNodes.end(), nodeId) == held->CandidateNodes.end())
        return false;
    const SplineNode* node = state.FindNode(nodeId);
    auto* overrides = world.GetComponentForWrite<SplineNetworkOverrides>(holder);
    State::Holder* holderState = state.Holders.Find(holder.id);
    if (!node || !overrides || !holderState)
        return false;
    const uint32 self = node->Members[0] == holder ? 0 : 1;
    overrides->Records[slot].ArcLengthMetres = node->ArcLengthMetres[self];
    overrides->Records[slot].OtherArcLengthMetres = node->ArcLengthMetres[1 - self];
    holderState->Written = *overrides;
    // Every other record bound to the node is held beside the kept one, by where the node is.
    for (auto& [id, other] : state.Holders)
        for (State::Binding& binding : other.Bindings)
            if (binding.NodeId == nodeId)
                binding = {0, node->Position, true};
    holderState->Bindings[slot] = {node->Id, node->Position, true};
    state.Dirty = true;
    return true;
}

void SplineNetwork::SetRecord(ECS::World& world, ECS::EntityHandle holder, uint32 slot,
                              const SplineNetworkOverride& record, const Vector3& position)
{
    assert(slot < Components::kMaxSplineNetworkOverrides);
    if (slot >= Components::kMaxSplineNetworkOverrides || !world.IsValid(holder))
        return;
    SplineNetworkOverrides overrides{};
    const auto* existing = world.GetComponent<SplineNetworkOverrides>(holder);
    if (existing)
        overrides = *existing;
    overrides.Records[slot] = record;
    std::unique_ptr<State>* found = m_States.Find(world.GetWorldId());
    const State::Holder* known = found ? (*found)->Holders.Find(holder.id) : nullptr;
    // Compared before the write below, which may move the component `existing` points at.
    const bool seen = known && existing && std::memcmp(&known->Written, existing, sizeof(*existing)) == 0;
    world.AddComponentImmediate(holder, overrides);
    if (!found)
        return;
    State& state = **found;
    State::Holder& holderState = state.Holders.GetOrInsert(holder.id);
    // A write the service has not seen yet is still a load for the other records.
    if (!seen)
        for (State::Binding& binding : holderState.Bindings)
            binding = {};
    holderState.Written = overrides;
    holderState.Bindings[slot] = record.Rule == SplineNodeRule::None ? State::Binding{} : State::Binding{0, position, true};
    state.Dirty = true;
}

} // namespace GameEngine::SplineECS
