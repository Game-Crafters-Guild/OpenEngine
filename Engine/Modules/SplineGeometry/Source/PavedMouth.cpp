#include "PavedMouth.h"

#include "ArmSideLine.h"
#include "PavedArmPath.h"
#include "RingWalk.h"

#include "Mathematics/Angle.h"
#include "Mathematics/Geometry.h"
#include "SplineGeometry/SplineCorner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;

namespace
{

// Arc points lie at most this far apart, the network's curve spacing, and turn at most
// kRoundCornerStepDegrees each.
constexpr float32 kMouthArcSpacingMeters = 0.25f;
// The step of the search for where a side leaves the outline, in meters.
constexpr float32 kMouthSearchStepMeters = 0.05f;
// The smallest step of the search for an arc's end on the side, in meters.
constexpr float32 kMouthMinFitStepMeters = 0.005f;
// Halvings that refine a crossing, and the radius of an arc that does not fit.
constexpr int kMouthBisections = 24;
constexpr int kMouthRadiusBisections = 12;
// How far a side is followed behind the node to meet the outline, in the arm's widths.
constexpr float32 kMouthReachBackWidths = 4.0f;
// How far along the arm past its corner an arc may end, in the arm's widths, when that is more than
// the arc's radius. A smaller arc fits a shallow entry within it.
constexpr float32 kMouthReachWidths = 3.0f;

// Where one side of an arm leaves an outline: the sharp corner a mouth rounds.
struct MouthCorner
{
    uint32 Arm = 0;
    uint32 Outline = 0;
    ArmSideKind Side = ArmSideKind::Right;
    // Meters along the arm; negative behind the node.
    float32 Along = 0.0f;
    RingSpot Spot;
    // The way along the outline, away from the arm: 1 in the ring's order, -1 against it.
    int Direction = 1;
    // Meters along the outline the arc may reach, and along the arm: to the arm's end, and past the
    // corner by ArmReach or the arc's radius, whichever is more.
    float32 OutlineBudget = 0.0f;
    float32 ArmLimit = 0.0f;
    float32 ArmReach = 0.0f;
};

// An arc that rounds a corner: it meets the arm's side `Along` meters along the arm and the outline
// at `Touch`.
struct MouthArc
{
    float32 Radius = 0.0f;
    float32 Along = 0.0f;
    Vector2 Center{};
    RingSpot Touch;
};

// Whether `arm` starts on `outline` and leaves it: its node lies within half its width of the
// outline, and the middle of its section one width out lies outside it.
bool LeavesOutline(const PavedArmPath& arm, const PavedTaggedRing& outline, const RingWalk& walk)
{
    const PavedArmSection& node = arm.GetSection(0);
    const Vector2 middle = (node.Left + node.Right) * 0.5f;
    if ((walk.Nearest(middle).Point - middle).Length() > std::max(node.Width * 0.5f, kPavedTouchMeters))
        return false;
    const PavedArmSection ahead = arm.SectionAt(std::min(arm.GetLength(), node.Width));
    return !Mathematics::PointInPolygon((ahead.Left + ahead.Right) * 0.5f, outline.Ring);
}

// Meters along the arm where `side` crosses `outline` nearest the node: ahead of the node when the
// side starts inside the outline, else behind it.
std::optional<float32> FindCrossing(const ArmSideLine& side, const PavedTaggedRing& outline, float32 ahead,
                                    float32 behind)
{
    const bool startsInside = Mathematics::PointInPolygon(side.At(0.0f), outline.Ring);
    const float32 step = startsInside ? kMouthSearchStepMeters : -kMouthSearchStepMeters;
    const float32 limit = startsInside ? ahead : behind;
    float32 previous = 0.0f;
    for (float32 along = step; std::abs(along) <= limit; along += step)
    {
        if (Mathematics::PointInPolygon(side.At(along), outline.Ring) == startsInside)
        {
            previous = along;
            continue;
        }
        float32 low = previous;
        float32 high = along;
        for (int i = 0; i < kMouthBisections; ++i)
        {
            const float32 middle = 0.5f * (low + high);
            (Mathematics::PointInPolygon(side.At(middle), outline.Ring) == startsInside ? low : high) = middle;
        }
        return 0.5f * (low + high);
    }
    return std::nullopt;
}

// Meters by which a circle of `radius`, `radius` out from `side` at `along`, clears the outline;
// negative while it overlaps the outline.
float32 ArcClearance(const ArmSideLine& side, const RingWalk& walk, float32 along, float32 radius)
{
    return walk.SignedDistance(side.At(along) + side.OutAt(along) * radius) - radius;
}

// The arc of `radius` that rounds `corner`, if it fits within the corner's budgets. Its center
// rides `radius` out from the side until it is `radius` from the outline.
std::optional<MouthArc> FitArc(const ArmSideLine& side, const RingWalk& walk, const MouthCorner& corner, float32 radius)
{
    const float32 armLimit = std::min(corner.ArmLimit, corner.Along + std::max(corner.ArmReach, radius));
    float32 low = corner.Along;
    float32 high = corner.Along;
    bool found = false;
    while (high < armLimit)
    {
        // The clearance changes by at most a meter per meter along a straight side: half its size is a
        // step that cannot pass the first fit on a gently curved one.
        const float32 value = ArcClearance(side, walk, high, radius);
        if (value >= 0.0f && high > corner.Along)
        {
            found = true;
            break;
        }
        low = high;
        high = std::min(armLimit, high + std::max(kMouthMinFitStepMeters, -0.5f * value));
        if (high == low)
            break;
    }
    if (!found)
        return std::nullopt;
    for (int i = 0; i < kMouthBisections; ++i)
    {
        const float32 middle = 0.5f * (low + high);
        (ArcClearance(side, walk, middle, radius) >= 0.0f ? high : low) = middle;
    }
    MouthArc arc;
    arc.Radius = radius;
    arc.Along = high;
    arc.Center = side.At(high) + side.OutAt(high) * radius;
    arc.Touch = walk.Nearest(arc.Center);
    const float32 onOutline = walk.Along(corner.Spot.Length, arc.Touch.Length, corner.Direction);
    if (onOutline > corner.OutlineBudget || onOutline > 0.5f * walk.GetPerimeter())
        return std::nullopt;
    return arc;
}

// The largest arc up to `radius` that fits `corner`, or none under kMinPavedMouthRadiusMeters.
std::optional<MouthArc> LargestArc(const ArmSideLine& side, const RingWalk& walk, const MouthCorner& corner,
                                   float32 radius)
{
    if (std::optional<MouthArc> arc = FitArc(side, walk, corner, radius))
        return arc;
    std::optional<MouthArc> best;
    float32 low = 0.0f;
    float32 high = radius;
    for (int i = 0; i < kMouthRadiusBisections; ++i)
    {
        const float32 middle = 0.5f * (low + high);
        if (std::optional<MouthArc> arc = FitArc(side, walk, corner, middle))
        {
            best = arc;
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    if (!best || best->Radius < kMinPavedMouthRadiusMeters)
        return std::nullopt;
    return best;
}

// The points of `arc` strictly between where it touches the outline and where it meets the side.
void AppendArc(const MouthArc& arc, const Vector2& onSide, AreaRing& out)
{
    const Vector2 from = arc.Touch.Point - arc.Center;
    const Vector2 to = onSide - arc.Center;
    const float32 start = std::atan2(from.y, from.x);
    const float32 sweep = Mathematics::SignedAngleDifference(start, std::atan2(to.y, to.x));
    const float32 stepRadians = kRoundCornerStepDegrees * std::numbers::pi_v<float32> / 180.0f;
    const int steps = std::max({1, static_cast<int>(std::ceil(std::abs(sweep) / stepRadians)),
                                static_cast<int>(std::ceil(std::abs(sweep) * arc.Radius / kMouthArcSpacingMeters))});
    for (int i = 1; i < steps; ++i)
    {
        const float32 angle = start + sweep * static_cast<float32>(i) / static_cast<float32>(steps);
        out.push_back(arc.Center + Vector2(std::cos(angle), std::sin(angle)) * arc.Radius);
    }
}

// The ring that fills `corner` up to `arc`. A side that starts outside the outline also fills the notch behind the
// node. The notch closes at the outline's point nearest `node` when the node is inside the outline, else where the
// node's cap crosses the outline, so no gap stays between the cap and the outline.
AreaRing FillRing(const ArmSideLine& side, const Vector2& node, const RingWalk& walk, const MouthCorner& corner,
                  const MouthArc& arc)
{
    AreaRing ring;
    ring.push_back(corner.Spot.Point);
    walk.AppendBetween(corner.Spot, arc.Touch, corner.Direction, ring);
    ring.push_back(arc.Touch.Point);
    const Vector2 onSide = side.At(arc.Along);
    AppendArc(arc, onSide, ring);
    ring.push_back(onSide);
    if (corner.Along >= 0.0f)
    {
        side.AppendBetween(arc.Along, corner.Along, ring);
        return ring;
    }
    side.AppendBetween(arc.Along, 0.0f, ring);
    const Vector2 capEnd = side.At(0.0f);
    ring.push_back(capEnd);
    std::optional<RingSpot> crossing;
    if (walk.SignedDistance(node) > 0.0f)
        crossing = walk.FirstCrossing(node, node * 2.0f - capEnd);
    const RingSpot entry = crossing ? *crossing : walk.Nearest(node);
    if ((entry.Point - node).Length() > kPavedTouchMeters)
        ring.push_back(node);
    ring.push_back(entry.Point);
    walk.AppendBetween(entry, corner.Spot, corner.Direction, ring);
    return ring;
}

} // namespace

std::vector<PavedMouth> BuildPavedMouths(std::span<const PavedArmPath> paths, std::span<const uint8> armValid,
                                         std::span<const PavedTaggedRing> outlines, std::span<const float32> radii)
{
    std::vector<PavedMouth> mouths(paths.size());
    std::vector<RingWalk> walks;
    walks.reserve(outlines.size());
    for (const PavedTaggedRing& outline : outlines)
        walks.emplace_back(outline.Ring);

    std::vector<MouthCorner> corners;
    for (uint32 arm = 0; arm < paths.size(); ++arm)
    {
        if (!armValid[arm])
            continue;
        const PavedArmPath& path = paths[arm];
        for (uint32 outline = 0; outline < outlines.size() && outline < radii.size(); ++outline)
        {
            if (!(radii[outline] > 0.0f) || outlines[outline].Ring.empty() ||
                !LeavesOutline(path, outlines[outline], walks[outline]))
                continue;
            const float32 behind = kMouthReachBackWidths * path.GetSection(0).Width;
            for (const ArmSideKind kind : {ArmSideKind::Right, ArmSideKind::Left})
            {
                const ArmSideLine side(path, kind);
                const std::optional<float32> along = FindCrossing(side, outlines[outline], path.GetLength(), behind);
                if (!along)
                    continue;
                MouthCorner corner;
                corner.Arm = arm;
                corner.Outline = outline;
                corner.Side = kind;
                corner.Along = *along;
                corner.Spot = walks[outline].Nearest(side.At(*along));
                corner.Direction = Vector2::Dot(walks[outline].Tangent(corner.Spot), side.OutAt(*along)) >= 0.0f ? 1 : -1;
                corner.ArmLimit = path.GetLength() - kPavedTouchMeters;
                corner.ArmReach = kMouthReachWidths * path.GetSection(0).Width;
                corners.push_back(corner);
            }
            break;
        }
    }

    // Each arc reaches at most half the way along the outline to the next corner, so two arcs meet
    // and never cross.
    for (MouthCorner& corner : corners)
    {
        const RingWalk& walk = walks[corner.Outline];
        float32 nearest = walk.GetPerimeter();
        for (const MouthCorner& other : corners)
        {
            if (&other == &corner || other.Outline != corner.Outline)
                continue;
            nearest = std::min(nearest, walk.Along(corner.Spot.Length, other.Spot.Length, corner.Direction));
        }
        corner.OutlineBudget = 0.5f * nearest;
    }

    for (const MouthCorner& corner : corners)
    {
        const PavedArmPath& path = paths[corner.Arm];
        const ArmSideLine side(path, corner.Side);
        const PavedArmSection& nodeSection = path.GetSection(0);
        const RingWalk& walk = walks[corner.Outline];
        const std::optional<MouthArc> arc = LargestArc(side, walk, corner, radii[corner.Outline]);
        if (!arc)
            continue;
        AreaRing ring = FillRing(side, (nodeSection.Left + nodeSection.Right) * 0.5f, walk, corner, *arc);
        if (ring.size() < 3)
            continue;
        if (Mathematics::PolygonDoubledSignedArea(ring) < 0.0f)
            std::reverse(ring.begin(), ring.end());
        PavedMouth& mouth = mouths[corner.Arm];
        mouth.Rings.push_back(TagRing(ring, PavedEdgeSource::Mouth, corner.Arm));
        mouth.Throat = std::max(mouth.Throat, arc->Along);
    }
    for (PavedMouth& mouth : mouths)
        mouth.BothSides = mouth.Rings.size() == 2;
    return mouths;
}

} // namespace GameEngine::SplineGeometry
