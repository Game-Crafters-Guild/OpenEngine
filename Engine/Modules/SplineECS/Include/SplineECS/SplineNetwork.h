#pragma once

#include "Components/Spline/SplineNetworkOverrides.h"
#include "ECS/Entity.h"
#include "Events/Event.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::SplineECS
{

// How far a node may move between two derivations, or a saved key may sit from its node, and
// still be the same node, in world meters. A drag moves a node by centimeters per edit epoch.
inline constexpr float32 kNodeMatchMetres = 10.0f;
// Two member splines touch when they come this close, in world meters: the layout's touching
// tolerance. The node rule reads no other distance and no setting.
inline constexpr float32 kNodeTouchMetres = 0.001f;
// Arms whose heights at a node differ by more than this are a bridge or a tunnel (SplitLevel),
// in meters. Provisional until a capture sets it.
inline constexpr float32 kSplitLevelToleranceMetres = 0.25f;

enum class SplineNodeKind : uint8
{
    // Two Roads' centerlines cross.
    Crossing = 0,
    // A Road's end lies on another Road's ribbon (a T), or an Attach override names the body.
    Attachment = 1,
    // A Road's centerline crosses an Area's outline or ends on it, or an Attach override names
    // the Area.
    Entry = 2,
    // Two Roads' ends meet end to end.
    Merge = 3,
};

/// One direction leaving a node along a member.
struct SplineNodeArm
{
    ECS::EntityHandle Member{};
    // +1 toward the member's end, -1 toward its start.
    int8 Direction = 1;
    // The arm's direction in the ground plane: atan2(Forward.z, Forward.x), radians in (-pi, pi].
    float32 AngleRadians = 0.0f;
    // Unit tangent along the arm, leaving the node.
    Mathematics::Vector3 Forward{};
};

inline constexpr uint32 kMaxSplineNodeArms = 4;

/// A meeting of two member splines. Derived on every change, never saved; its id is stable while
/// the service can follow the meeting (a drag, a knot insert, an upstream edit) and is never
/// reused for another meeting of its world.
struct SplineNode
{
    uint32 Id = 0;
    SplineNodeKind Kind = SplineNodeKind::Crossing;
    // For an attachment and an entry, Members[1] is the body or the Area; for a merge and a
    // crossing the order carries no meaning.
    ECS::EntityHandle Members[2] = {};
    // The node along each member, world meters from that member's start.
    Mathematics::Vector2 ArcLengthMetres{};
    // Where the members' sampled centerlines meet: a crossing lies off the curves by up to the
    // chords' sag divided by the sine of the angle the two members cross at.
    Mathematics::Vector3 Position{};
    // Sorted by AngleRadians ascending: counter-clockwise seen from above (+Y), with +X right
    // and +Z up the screen.
    uint32 ArmCount = 0;
    SplineNodeArm Arms[kMaxSplineNodeArms] = {};
    // The two members' heights at the node, the second minus the first, in meters.
    float32 HeightDifferenceMetres = 0.0f;
    // The rule a record gives this node; None when no record matched it.
    Components::SplineNodeRule Rule = Components::SplineNodeRule::None;
    ECS::EntityHandle RuleHolder{};
    uint32 RuleSlot = 0;

    /// The heights differ beyond kSplitLevelToleranceMetres and no AtGrade rule joins them, or a
    /// SplitLevel rule keeps them apart: no consumer joins the node at grade.
    bool IsSplitLevel() const;
};

/// A record no node matched: kept in its component with its last keys and reported.
struct SplineHeldRecord
{
    ECS::EntityHandle Holder{};
    uint32 Slot = 0;
    Components::SplineNetworkOverride Record{};
    // Where its node was last seen, or where its saved keys place it.
    Mathematics::Vector3 LastPosition{};
    // Nodes of its pair within kNodeMatchMetres that it cannot choose between, or the node a
    // second record also claims; empty when nothing is near. Keep lands it on one.
    std::vector<uint32> CandidateNodes;
};

/// Why the network ignores a member.
enum class SplineMemberIssueKind : uint8
{
    // An Area member whose spline is not closed.
    OpenArea = 0,
};

/// A member the network ignores, and why.
struct SplineMemberIssue
{
    ECS::EntityHandle Member{};
    SplineMemberIssueKind Kind = SplineMemberIssueKind::OpenArea;
};

enum class SplineNodeChangeKind : uint8
{
    Added = 0,
    Changed = 1,
    Removed = 2,
};

struct SplineNodeChange
{
    uint32 NodeId = 0;
    SplineNodeChangeKind Kind = SplineNodeChangeKind::Added;
};

/// The spline network: derives the nodes among the member splines (SplineNetworkMember) of each
/// world, keeps their identities across edits and reloads, and matches the authored records
/// (SplineNetworkOverrides) to them. It generates no geometry. Each world has its own nodes and
/// identities; every query names the world. The component is the one copy of the records' keys:
/// the service re-keys it on every match, and any other write to it (a load, an undo, a
/// duplicate, a snapshot, a script) is a load for that entity's records, matched again by their
/// saved arc lengths. A record is matched by position, held when nothing is near and never
/// reassigned: a match is made only when it is mutual and unique.
class SplineNetwork
{
public:
    static void Initialize();
    static void Shutdown();
    static SplineNetwork& Get();
    static SplineNetwork* TryGet();
    static bool IsInitialized();

    SplineNetwork();
    ~SplineNetwork();

    /// Re-derive the world's nodes when a member, its spline, its transform or its records
    /// changed, then raise NodesChanged with the nodes that were added, changed or removed.
    /// Re-keys the records in the components. Runs after SplineExtraction and TransformHierarchy.
    void Update(ECS::World& world);

    /// The world's nodes at its last Update; empty for a world never updated.
    std::span<const SplineNode> GetNodes(const ECS::World& world) const;
    const SplineNode* FindNode(const ECS::World& world, uint32 id) const;
    /// The nodes on `member`, on either side of the pair.
    std::vector<const SplineNode*> FindNodes(const ECS::World& world, ECS::EntityHandle member) const;
    std::span<const SplineHeldRecord> GetHeldRecords(const ECS::World& world) const;
    std::span<const SplineMemberIssue> GetMemberIssues(const ECS::World& world) const;

    /// Lands a held record on one of its candidate nodes: re-keys it to that node. Every other
    /// record bound to that node is held beside it, with the node as its candidate. Returns false
    /// when the record is not held or the node is not one of its candidates.
    bool KeepRecord(ECS::World& world, ECS::EntityHandle holder, uint32 slot, uint32 nodeId);

    /// Writes one record as an undo step writes it: the record in `slot` of `holder` becomes
    /// `record` (Rule None empties the slot), and the next derivation matches it by position from
    /// `position`, where its junction was when the step was journaled, instead of as a load. The
    /// holder's other records keep their bindings, so the write disturbs none of them. Adds the
    /// component when the holder has none.
    void SetRecord(ECS::World& world, ECS::EntityHandle holder, uint32 slot,
                   const Components::SplineNetworkOverride& record, const Mathematics::Vector3& position);

    /// Raised after every derivation of a world that added, changed or removed a node, on the
    /// thread that ran Update.
    Event<const ECS::World&, std::span<const SplineNodeChange>> NodesChanged;

private:
    struct State;
    const State* FindState(const ECS::World& world) const;

    // By world id.
    FlatMap<uint64, std::unique_ptr<State>> m_States;
    // Shared by every world, so a node id is never issued twice.
    uint32 m_NextNodeId = 1;

    static std::unique_ptr<SplineNetwork> s_Instance;
};

} // namespace GameEngine::SplineECS
