#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// What a member spline is to the network.
enum class SplineNetworkMemberKind : uint8
{
    // A ribbon: the width band (the per-point Radius on each side) carries traffic. Open or
    // closed: a ring road is a Road.
    Road = 0,
    // A closed outline whose interior is the body, such as a square. An open Area is reported
    // and ignored.
    Area = 1,
};

/// Makes the entity's spline a member of the spline network: nodes (crossings, T junctions,
/// entries into an area, end-to-end merges) form only between members, where they touch within
/// 1 mm or cross. A spline without this component (a river, a lake, a fence, a mark-up region)
/// meets every road with no node. Add it to a road, path or square spline to make the spline a
/// network member.
struct SplineNetworkMember
{
    SplineNetworkMemberKind Kind = SplineNetworkMemberKind::Road;  // @ge-tooltip "Road: a strip that carries traffic, open or a ring. Area: a closed outline that roads enter, such as a square."
};

static_assert(std::is_trivially_copyable_v<SplineNetworkMember>, "SplineNetworkMember must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineNetworkMember>, "SplineNetworkMember must be standard layout for ECS storage");

} // namespace GameEngine::Components
