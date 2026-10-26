#include "Network/SplineNetworkDetect.h"

#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector2.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace GameEngine::SplineECS::Network
{
namespace
{

using Components::SplineNetworkMemberKind;
using Mathematics::ClosestPointOnSegment;
using Mathematics::GroundDistance;
using Mathematics::GroundPoint;
using Mathematics::Vector2;
using Mathematics::Vector3;

// World meters between centerline samples, for the broad phase and the crossings. The touch
// tests do not read the chords: NearestPointXZ refines on the curve itself.
constexpr float32 kSampleSpacingMetres = 0.25f;
constexpr uint32 kSamplesPerSegment = 8;
constexpr uint32 kMaxSamples = 8192;
// Golden-section steps over the three chords around the nearest one: 0.618^24 of the bracket,
// under 0.01 mm at the 0.25 m spacing.
constexpr uint32 kNearestRefineSteps = 24;
constexpr float32 kInverseGoldenRatio = 0.618034f;

// The largest ground-plane scale of the transform's basis: the factor from local to world meters
// for arc lengths and half-widths.
float32 GroundScale(const Mathematics::Matrix4x4& worldMatrix)
{
    const Vector3 origin = worldMatrix.TransformPoint({0.0f, 0.0f, 0.0f});
    const float32 x = (worldMatrix.TransformPoint({1.0f, 0.0f, 0.0f}) - origin).Length();
    const float32 z = (worldMatrix.TransformPoint({0.0f, 0.0f, 1.0f}) - origin).Length();
    return std::max(x, z);
}

bool BoundsOverlapXZ(const Mathematics::AABB& a, const Mathematics::AABB& b, float32 margin)
{
    return a.min.x <= b.max.x + margin && b.min.x <= a.max.x + margin && a.min.z <= b.max.z + margin &&
           b.min.z <= a.max.z + margin;
}

// The world point of `curve`'s spline at `arcLength` world meters from its start.
Vector3 CurvePointAt(const MemberCurve& curve, float32 arcLength)
{
    const Spline::SplineFrame frame = Spline::EvaluateAtDistance(*curve.Spline, curve.Wrap(arcLength) / curve.Scale);
    return curve.WorldMatrix.TransformPoint(frame.Position);
}

float32 GroundDistanceToCurve(const MemberCurve& curve, const Vector2& point, float32 arcLength)
{
    return (GroundPoint(CurvePointAt(curve, arcLength)) - point).Length();
}

// The nearest point on the spline within the chords before and after `segment`, the nearest
// chord. On a closed curve the bracket runs on across the start.
CurvePoint RefineOnCurve(const MemberCurve& curve, const Vector2& point, size_t segment)
{
    const size_t last = curve.Points.size() - 1;
    float32 low = segment > 0 ? curve.ArcLengths[segment - 1] : 0.0f;
    float32 high = segment + 2 <= last ? curve.ArcLengths[segment + 2] : curve.Length;
    if (curve.Closed && segment == 0 && last > 1)
        low = curve.ArcLengths[last - 1] - curve.Length;
    if (curve.Closed && segment + 2 > last && last > 1)
        high = curve.Length + curve.ArcLengths[1];
    float32 inner = Math::Lerp(high, low, kInverseGoldenRatio);
    float32 outer = Math::Lerp(low, high, kInverseGoldenRatio);
    float32 innerDistance = GroundDistanceToCurve(curve, point, inner);
    float32 outerDistance = GroundDistanceToCurve(curve, point, outer);
    for (uint32 step = 0; step < kNearestRefineSteps; ++step)
    {
        if (innerDistance < outerDistance)
        {
            high = outer;
            outer = inner;
            outerDistance = innerDistance;
            inner = Math::Lerp(high, low, kInverseGoldenRatio);
            innerDistance = GroundDistanceToCurve(curve, point, inner);
        }
        else
        {
            low = inner;
            inner = outer;
            innerDistance = outerDistance;
            outer = Math::Lerp(low, high, kInverseGoldenRatio);
            outerDistance = GroundDistanceToCurve(curve, point, outer);
        }
    }
    CurvePoint nearest;
    nearest.ArcLength = curve.Wrap((low + high) * 0.5f);
    nearest.Point = CurvePointAt(curve, nearest.ArcLength);
    nearest.Distance = (GroundPoint(nearest.Point) - point).Length();
    return nearest;
}

bool IsOpenRoad(const MemberCurve& curve)
{
    return curve.Kind == SplineNetworkMemberKind::Road && !curve.Closed;
}

// Whether a meeting in out[begin, end) lies within `reach` of `position` in the ground plane.
bool NearFound(const std::vector<RawNode>& out, size_t begin, size_t end, const Vector3& position, float32 reach)
{
    for (size_t i = begin; i < end; ++i)
        if (GroundDistance(out[i].Position, position) <= reach)
            return true;
    return false;
}

size_t SegmentIndexAt(const MemberCurve& curve, float32 arc)
{
    const auto it = std::upper_bound(curve.ArcLengths.begin(), curve.ArcLengths.end(), arc);
    return std::clamp<size_t>(static_cast<size_t>(it - curve.ArcLengths.begin()), 1, curve.Points.size() - 1);
}

float32 FractionAt(const MemberCurve& curve, size_t upper, float32 arc)
{
    const float32 span = curve.ArcLengths[upper] - curve.ArcLengths[upper - 1];
    return span > 0.0f ? (arc - curve.ArcLengths[upper - 1]) / span : 0.0f;
}

// Meetings at an end of the open Road `road`: on the body's ribbon (an attachment), on an
// Area's outline (an entry), or at one of the body's own ends (a merge).
void AddEndNodes(std::span<const MemberCurve* const> curves, uint32 road, uint32 body, size_t begin,
                 std::vector<RawNode>& out)
{
    const MemberCurve& end = *curves[road];
    const MemberCurve& other = *curves[body];
    if (!IsOpenRoad(end))
        return;
    for (const float32 endArc : {0.0f, end.Length})
    {
        const Vector3 endPoint = end.PointAt(endArc);
        const Mathematics::AABB endBounds{endPoint, endPoint};
        if (!BoundsOverlapXZ(endBounds, other.Bounds, kNodeTouchMetres))
            continue;
        const CurvePoint onOther = NearestPointXZ(other, endPoint);
        RawNode node;
        node.Members[0] = road;
        node.Members[1] = body;
        node.ArcLengths[0] = endArc;
        node.ArcLengths[1] = onOther.ArcLength;
        node.Position = onOther.Point;
        if (other.Kind == SplineNetworkMemberKind::Area)
        {
            if (onOther.Distance > kNodeTouchMetres)
                continue;
            node.Kind = SplineNodeKind::Entry;
        }
        else
        {
            if (onOther.Distance > other.HalfWidthAt(onOther.ArcLength) + kNodeTouchMetres)
                continue;
            node.Kind = SplineNodeKind::Attachment;
            for (const float32 otherEnd : {0.0f, other.Length})
            {
                if (!IsOpenRoad(other))
                    break;
                const Vector3 otherPoint = other.PointAt(otherEnd);
                if (GroundDistance(otherPoint, endPoint) > kNodeTouchMetres)
                    continue;
                node.Kind = SplineNodeKind::Merge;
                node.ArcLengths[1] = otherEnd;
                node.Position = (endPoint + otherPoint) * 0.5f;
            }
        }
        // The other side's pass finds the same two ends meeting.
        if (node.Kind == SplineNodeKind::Merge && NearFound(out, begin, out.size(), node.Position, kNodeTouchMetres))
            continue;
        out.push_back(node);
    }
}

// Where the centerline of `first` crosses the centerline (or the outline) of `second`. A crossing
// inside the ribbon of an end meeting already found is that end's meeting.
void AddCrossings(std::span<const MemberCurve* const> curves, uint32 first, uint32 second, size_t begin,
                  std::vector<RawNode>& out)
{
    const MemberCurve& a = *curves[first];
    const MemberCurve& b = *curves[second];
    const size_t endNodes = out.size();
    const SplineNodeKind kind =
        b.Kind == SplineNetworkMemberKind::Area ? SplineNodeKind::Entry : SplineNodeKind::Crossing;
    for (size_t runA = 0; runA < a.RunBounds.size(); ++runA)
    {
        if (!BoundsOverlapXZ(a.RunBounds[runA], b.Bounds, kNodeTouchMetres))
            continue;
        for (size_t runB = 0; runB < b.RunBounds.size(); ++runB)
        {
            if (!BoundsOverlapXZ(a.RunBounds[runA], b.RunBounds[runB], kNodeTouchMetres))
                continue;
            const size_t lastA = std::min((runA + 1) * kSamplesPerBound, a.Points.size() - 1);
            const size_t lastB = std::min((runB + 1) * kSamplesPerBound, b.Points.size() - 1);
            for (size_t i = runA * kSamplesPerBound; i < lastA; ++i)
            {
                for (size_t j = runB * kSamplesPerBound; j < lastB; ++j)
                {
                    const std::optional<Vector2> hit = Mathematics::SegmentIntersectionFractions(
                        GroundPoint(a.Points[i]), GroundPoint(a.Points[i + 1]), GroundPoint(b.Points[j]),
                        GroundPoint(b.Points[j + 1]));
                    if (!hit)
                        continue;
                    RawNode node;
                    node.Kind = kind;
                    node.Members[0] = first;
                    node.Members[1] = second;
                    node.ArcLengths[0] = a.ArcLengths[i] + hit->x * (a.ArcLengths[i + 1] - a.ArcLengths[i]);
                    node.ArcLengths[1] = b.ArcLengths[j] + hit->y * (b.ArcLengths[j + 1] - b.ArcLengths[j]);
                    node.Position = a.Points[i] + (a.Points[i + 1] - a.Points[i]) * hit->x;
                    const float32 reach =
                        std::max(a.HalfWidthAt(node.ArcLengths[0]), b.HalfWidthAt(node.ArcLengths[1]));
                    if (NearFound(out, begin, endNodes, node.Position, reach + kNodeTouchMetres))
                        continue;
                    // A crossing through a shared sample point is found by both segments.
                    if (NearFound(out, endNodes, out.size(), node.Position, kNodeTouchMetres))
                        continue;
                    out.push_back(node);
                }
            }
        }
    }
}

} // namespace

CurvePoint NearestPointXZ(const MemberCurve& curve, const Vector3& point)
{
    CurvePoint best;
    size_t bestSegment = 0;
    const Vector2 p = GroundPoint(point);
    const Vector3 ground{point.x, 0.0f, point.z};
    for (size_t run = 0; run < curve.RunBounds.size(); ++run)
    {
        const Mathematics::AABB& bounds = curve.RunBounds[run];
        const float32 dx = std::max({bounds.min.x - p.x, 0.0f, p.x - bounds.max.x});
        const float32 dz = std::max({bounds.min.z - p.y, 0.0f, p.y - bounds.max.z});
        if (std::sqrt(dx * dx + dz * dz) > best.Distance)
            continue;
        const size_t first = run * kSamplesPerBound;
        const size_t last = std::min(first + kSamplesPerBound, curve.Points.size() - 1);
        for (size_t i = first; i < last; ++i)
        {
            const Vector3 a{curve.Points[i].x, 0.0f, curve.Points[i].z};
            const Vector3 b{curve.Points[i + 1].x, 0.0f, curve.Points[i + 1].z};
            const Vector3 closest = ClosestPointOnSegment(ground, a, b);
            const float32 chord = GroundDistance(a, b);
            const float32 s = chord > 0.0f ? GroundDistance(a, closest) / chord : 0.0f;
            const float32 distance = GroundDistance(closest, ground);
            if (distance < best.Distance)
            {
                best.Distance = distance;
                best.ArcLength = curve.ArcLengths[i] + s * (curve.ArcLengths[i + 1] - curve.ArcLengths[i]);
                best.Point = curve.Points[i] + (curve.Points[i + 1] - curve.Points[i]) * s;
                bestSegment = i;
            }
        }
    }
    if (!curve.Spline || curve.Scale <= 0.0f || best.Distance == std::numeric_limits<float32>::max())
        return best;
    return RefineOnCurve(curve, p, bestSegment);
}

Vector3 MemberCurve::PointAt(float32 arcLength) const
{
    const float32 arc = Wrap(arcLength);
    const size_t upper = SegmentIndexAt(*this, arc);
    return Points[upper - 1] + (Points[upper] - Points[upper - 1]) * FractionAt(*this, upper, arc);
}

Vector3 MemberCurve::TangentAt(float32 arcLength) const
{
    const size_t upper = SegmentIndexAt(*this, Wrap(arcLength));
    const Vector3 chord = Points[upper] - Points[upper - 1];
    const float32 length = chord.Length();
    return length > 0.0f ? chord * (1.0f / length) : Vector3{1.0f, 0.0f, 0.0f};
}

float32 MemberCurve::HalfWidthAt(float32 arcLength) const
{
    const float32 arc = Wrap(arcLength);
    const size_t upper = SegmentIndexAt(*this, arc);
    return HalfWidths[upper - 1] + (HalfWidths[upper] - HalfWidths[upper - 1]) * FractionAt(*this, upper, arc);
}

float32 MemberCurve::Wrap(float32 arcLength) const
{
    if (!Closed || Length <= 0.0f)
        return std::clamp(arcLength, 0.0f, Length);
    const float32 wrapped = std::fmod(arcLength, Length);
    return wrapped < 0.0f ? wrapped + Length : wrapped;
}

MemberCurve SampleMemberCurve(ECS::EntityHandle entity, SplineNetworkMemberKind kind, const Spline::SplineData& spline,
                              const Mathematics::Matrix4x4& worldMatrix)
{
    MemberCurve curve;
    curve.Entity = entity;
    curve.Kind = kind;
    curve.Closed = spline.IsEffectivelyClosed();
    const float32 scale = GroundScale(worldMatrix);
    const float32 localLength = spline.TotalArcLength;
    // Arc lengths are the spline's own arc-length table scaled to world meters, as the layout
    // scales every spline length, so a saved key means one place to every consumer.
    curve.Length = localLength * scale;
    curve.Spline = &spline;
    curve.WorldMatrix = worldMatrix;
    curve.Scale = scale;
    const uint32 bySpacing = static_cast<uint32>(std::ceil(localLength * scale / kSampleSpacingMetres));
    const uint32 intervals =
        std::clamp(std::max(bySpacing, spline.GetSegmentCount() * kSamplesPerSegment), 1u, kMaxSamples);
    curve.Points.reserve(intervals + 1);
    curve.ArcLengths.reserve(intervals + 1);
    curve.HalfWidths.reserve(intervals + 1);
    for (uint32 i = 0; i <= intervals; ++i)
    {
        const float32 distance = localLength * static_cast<float32>(i) / static_cast<float32>(intervals);
        const Spline::SplineFrame frame = Spline::EvaluateAtDistance(spline, distance);
        const Vector3 point = worldMatrix.TransformPoint(frame.Position);
        curve.Points.push_back(point);
        curve.ArcLengths.push_back(distance * scale);
        curve.HalfWidths.push_back(std::max(frame.Radius, 0.0f) * scale);
    }
    for (size_t first = 0; first + 1 < curve.Points.size(); first += kSamplesPerBound)
    {
        Mathematics::AABB bounds = Mathematics::AABB::Empty();
        float32 halfWidth = 0.0f;
        const size_t last = std::min(first + kSamplesPerBound, curve.Points.size() - 1);
        for (size_t i = first; i <= last; ++i)
        {
            bounds.Expand(curve.Points[i]);
            halfWidth = std::max(halfWidth, curve.HalfWidths[i]);
        }
        bounds.min = bounds.min - Vector3{halfWidth, 0.0f, halfWidth};
        bounds.max = bounds.max + Vector3{halfWidth, 0.0f, halfWidth};
        curve.RunBounds.push_back(bounds);
        curve.Bounds.Expand(bounds);
    }
    return curve;
}

void OverlappingPairs(std::span<const MemberCurve* const> curves, std::vector<MemberPair>& out)
{
    // Sort and sweep along X: each curve is tested against the curves after it in order of their
    // smallest X, up to the first that starts beyond its largest X.
    std::vector<uint32> order;
    order.reserve(curves.size());
    for (uint32 i = 0; i < curves.size(); ++i)
        if (!curves[i]->RunBounds.empty())
            order.push_back(i);
    std::sort(order.begin(), order.end(),
              [curves](uint32 left, uint32 right) { return curves[left]->Bounds.min.x < curves[right]->Bounds.min.x; });
    const size_t begin = out.size();
    for (size_t k = 0; k < order.size(); ++k)
    {
        const Mathematics::AABB& bounds = curves[order[k]]->Bounds;
        for (size_t next = k + 1; next < order.size(); ++next)
        {
            const Mathematics::AABB& other = curves[order[next]]->Bounds;
            if (other.min.x > bounds.max.x + kNodeTouchMetres)
                break;
            if (BoundsOverlapXZ(bounds, other, kNodeTouchMetres))
                out.push_back({std::min(order[k], order[next]), std::max(order[k], order[next])});
        }
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(begin), out.end());
}

void DetectPairNodes(std::span<const MemberCurve* const> curves, uint32 first, uint32 second, std::vector<RawNode>& out)
{
    const MemberCurve& a = *curves[first];
    const MemberCurve& b = *curves[second];
    if (a.RunBounds.empty() || b.RunBounds.empty() || !BoundsOverlapXZ(a.Bounds, b.Bounds, kNodeTouchMetres))
        return;
    const bool aArea = a.Kind == SplineNetworkMemberKind::Area;
    const bool bArea = b.Kind == SplineNetworkMemberKind::Area;
    // Two Areas sharing an edge form no node, and an open Area is ignored.
    if ((aArea && bArea) || (aArea && !a.Closed) || (bArea && !b.Closed))
        return;
    const size_t begin = out.size();
    const uint32 road = aArea ? second : first;
    const uint32 other = aArea ? first : second;
    AddEndNodes(curves, road, other, begin, out);
    if (!aArea && !bArea)
        AddEndNodes(curves, other, road, begin, out);
    AddCrossings(curves, road, other, begin, out);
}

void BuildArms(const MemberCurve& first, const MemberCurve& second, SplineNode& node)
{
    node.ArmCount = 0;
    const MemberCurve* curves[2] = {&first, &second};
    for (uint32 m = 0; m < 2; ++m)
    {
        const MemberCurve& curve = *curves[m];
        const float32 arc = node.ArcLengthMetres[m];
        const Vector3 tangent = curve.TangentAt(arc);
        const Vector3 ground{tangent.x, 0.0f, tangent.z};
        const float32 groundLength = ground.Length();
        const Vector3 forward = groundLength > 0.0f ? ground * (1.0f / groundLength) : Vector3{1.0f, 0.0f, 0.0f};
        const bool hasForward = curve.Closed || arc < curve.Length - kNodeTouchMetres;
        const bool hasBackward = curve.Closed || arc > kNodeTouchMetres;
        for (const int8 direction : {int8{1}, int8{-1}})
        {
            if ((direction > 0 && !hasForward) || (direction < 0 && !hasBackward))
                continue;
            SplineNodeArm& arm = node.Arms[node.ArmCount++];
            arm.Member = curve.Entity;
            arm.Direction = direction;
            arm.Forward = forward * static_cast<float32>(direction);
            arm.AngleRadians = std::atan2(arm.Forward.z, arm.Forward.x);
            // -X with a negative-zero z comes back as -pi; the range is (-pi, pi].
            if (arm.AngleRadians <= -std::numbers::pi_v<float32>)
                arm.AngleRadians = std::numbers::pi_v<float32>;
        }
    }
    std::sort(node.Arms, node.Arms + node.ArmCount,
              [](const SplineNodeArm& l, const SplineNodeArm& r) { return l.AngleRadians < r.AngleRadians; });
    node.HeightDifferenceMetres =
        second.PointAt(node.ArcLengthMetres[1]).y - first.PointAt(node.ArcLengthMetres[0]).y;
}

} // namespace GameEngine::SplineECS::Network
