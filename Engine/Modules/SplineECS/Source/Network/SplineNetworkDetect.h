#pragma once

#include "SplineECS/SplineNetwork.h"

#include "Components/Spline/SplineNetworkMember.h"
#include "ECS/Entity.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"

#include <compare>
#include <limits>
#include <span>
#include <vector>

namespace GameEngine::Spline
{
class SplineData;
}

namespace GameEngine::SplineECS::Network
{

// A member's centerline sampled in world space, with the arc length of every sample in world
// meters and the half-width (the per-point Radius, scaled) at it.
struct MemberCurve
{
    ECS::EntityHandle Entity{};
    Components::SplineNetworkMemberKind Kind = Components::SplineNetworkMemberKind::Road;
    bool Closed = false;
    std::vector<Mathematics::Vector3> Points;
    std::vector<float32> ArcLengths;
    std::vector<float32> HalfWidths;
    float32 Length = 0.0f;
    // The XZ bounds of each run of kSamplesPerBound segments, grown by the run's half-width.
    std::vector<Mathematics::AABB> RunBounds;
    Mathematics::AABB Bounds = Mathematics::AABB::Empty();
    // The spline the samples came from and its placement, for the nearest-point query on the
    // curve itself. The spline is set again at every Update, so it is valid during one.
    const Spline::SplineData* Spline = nullptr;
    Mathematics::Matrix4x4 WorldMatrix{};
    // World meters per local meter of arc length.
    float32 Scale = 1.0f;

    Mathematics::Vector3 PointAt(float32 arcLength) const;
    Mathematics::Vector3 TangentAt(float32 arcLength) const;
    float32 HalfWidthAt(float32 arcLength) const;
    // Arc length wrapped into [0, Length) on a closed curve, clamped to [0, Length] on an open one.
    float32 Wrap(float32 arcLength) const;
};

inline constexpr uint32 kSamplesPerBound = 16;

MemberCurve SampleMemberCurve(ECS::EntityHandle entity, Components::SplineNetworkMemberKind kind,
                              const Spline::SplineData& spline, const Mathematics::Matrix4x4& worldMatrix);

// The nearest point of a curve to a point, measured in the ground plane.
struct CurvePoint
{
    // World meters from the curve's start.
    float32 ArcLength = 0.0f;
    // Ground-plane distance to the queried point, in world meters.
    float32 Distance = std::numeric_limits<float32>::max();
    Mathematics::Vector3 Point{};
};

// The nearest point of `curve` to `point` in the ground plane, on the spline itself: the nearest
// chord of the samples, refined on the curve around it, so a point on a bend between two samples
// is at distance zero.
CurvePoint NearestPointXZ(const MemberCurve& curve, const Mathematics::Vector3& point);

// A meeting the geometry makes, before identity: the members by index into the curves.
struct RawNode
{
    SplineNodeKind Kind = SplineNodeKind::Crossing;
    uint32 Members[2] = {};
    Mathematics::Vector2 ArcLengths{};
    Mathematics::Vector3 Position{};
    // The slot of the Attach record on Members[0] that made the node, or -1 for a meeting the
    // geometry makes.
    int32 AttachSlot = -1;
};

// Two curves by index, the lower index first.
struct MemberPair
{
    uint32 First = 0;
    uint32 Second = 0;

    friend auto operator<=>(const MemberPair&, const MemberPair&) = default;
};

// Every pair of curves whose ground-plane bounds overlap within kNodeTouchMetres, the only pairs
// the geometry can make a meeting for, appended to `out` in ascending order.
void OverlappingPairs(std::span<const MemberCurve* const> curves, std::vector<MemberPair>& out);

// Every meeting between two curves: crossings, ends on a ribbon or an outline, ends meeting.
// For an attachment and an entry, Members[1] is the body.
void DetectPairNodes(std::span<const MemberCurve* const> curves, uint32 first, uint32 second, std::vector<RawNode>& out);

// The arms of a node, sorted counter-clockwise seen from above, and the members' height
// difference at it.
void BuildArms(const MemberCurve& first, const MemberCurve& second, SplineNode& node);

} // namespace GameEngine::SplineECS::Network
