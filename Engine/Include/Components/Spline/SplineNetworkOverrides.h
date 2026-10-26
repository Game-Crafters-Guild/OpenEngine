#pragma once

#include "ECS/Entity.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// The authored rule of one network node. None marks an empty slot.
enum class SplineNodeRule : uint8
{
    None = 0,
    // Join at grade although the arms' heights differ by more than the level check allows.
    AtGrade = 1,
    // Keep the two members apart although they meet: a bridge or a tunnel.
    SplitLevel = 2,
    // Closed to traffic. No consumer reads it yet.
    Closed = 3,
    // This member's end nearest ArcLengthMetres attaches to Other at OtherArcLengthMetres as
    // if it lay there: the network makes an attachment, or an entry when Other is an Area.
    Attach = 4,
};

/// One override, keyed by the node it rides on: the other member and the arc length of the
/// node along each member, in world meters from that member's start (modulo the loop on a
/// closed member). The spline network re-keys both arc lengths on every match.
struct SplineNetworkOverride
{
    // The other member of the node. A handle that names no live member holds the record.
    ECS::EntityHandle Other{};
    float32 ArcLengthMetres = 0.0f;
    float32 OtherArcLengthMetres = 0.0f;
    SplineNodeRule Rule = SplineNodeRule::None;
    uint8 _Pad[3] = {};  // @ge-hidden

    bool operator==(const SplineNetworkOverride&) const = default;
};

// The most overrides one member holds.
inline constexpr uint32 kMaxSplineNetworkOverrides = 32;

/// The authored topology of a network member: per-node rules, looked up on either member of
/// the node. Every slot whose Rule is not None is one record, in any order. Nodes are derived
/// and never saved; these records are the only saved network state. A record no node matches
/// is held, kept here with its last keys and reported, never moved to another node. A filled
/// slot writes one scene line per member ("Records3.Rule = AtGrade"); an empty table writes none.
// @ge-no-add        written by the junction commands (Attach, Detach, Join at Grade, Clear, Keep), never by hand
struct SplineNetworkOverrides
{
    SplineNetworkOverride Records[kMaxSplineNetworkOverrides];
};

static_assert(sizeof(SplineNetworkOverride) == 16, "SplineNetworkOverride is a 4-byte handle, two arc lengths and a rule, padded");
static_assert(std::is_trivially_copyable_v<SplineNetworkOverrides>, "SplineNetworkOverrides must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineNetworkOverrides>, "SplineNetworkOverrides must be standard layout for ECS storage");

} // namespace GameEngine::Components
