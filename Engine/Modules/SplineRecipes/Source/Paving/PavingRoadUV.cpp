#include "PavingRoadUV.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "SplineGeometry/PavedFootprint.h"
#include "SplineRecipes/Paving/PavingPlan.h"
#include "SplineRecipes/Paving/PavingSources.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;

// Two centerline points whose arc lengths differ by less than this, in meters, are one point.
constexpr float32 kSameStationMeters = 1.0e-3f;

// A segment shorter than this on the ground, in meters, has no direction.
constexpr float32 kMinSegmentMeters = 1.0e-4f;

// One road's arms in a region, gathered before its centerline is built.
struct RoadArms
{
    uint32 Road = 0;
    PavingRank Rank;
    std::vector<SG::PavedArmStation> Stations;
    std::vector<Vector2> Spans;
    // The top's half-widths at the first arm's stop, left and right of rising travel.
    float32 CapHalfLeft = 0.0f;
    float32 CapHalfRight = 0.0f;
    float32 RoadLoopMeters = 0.0f;
    uint32 ArmCount = 0;
};

// `distance` on a closed road of `loopMeters`, moved by whole laps to within half a lap of `node`.
float32 Unwrapped(float32 distance, float32 node, float32 loopMeters)
{
    return loopMeters > 0.0f ? node + std::remainder(distance - node, loopMeters) : distance;
}

SG::PavedArmStation Interpolated(const SG::PavedArmStation& a, const SG::PavedArmStation& b, float32 t)
{
    SG::PavedArmStation station;
    station.Position = a.Position + (b.Position - a.Position) * t;
    station.HalfWidthLeft = Math::Lerp(a.HalfWidthLeft, b.HalfWidthLeft, t);
    station.HalfWidthRight = Math::Lerp(a.HalfWidthRight, b.HalfWidthRight, t);
    station.Distance = Math::Lerp(a.Distance, b.Distance, t);
    return station;
}

// The stations of `arm` from its node to its stop, the last one at the stop, their distances
// unwrapped about the node and their half-widths turned to rising travel.
std::vector<SG::PavedArmStation> StationsToStop(const SG::PavedArm& arm, const SG::PavedArmStop& stop)
{
    std::vector<SG::PavedArmStation> stations;
    const float32 loop = arm.RoadLoopMeters;
    const float32 reach = std::abs(Unwrapped(stop.StopDistance, stop.NodeDistance, loop) - stop.NodeDistance);
    for (SG::PavedArmStation station : arm.Stations)
    {
        station.Distance = Unwrapped(station.Distance, stop.NodeDistance, loop);
        const float32 along = std::abs(station.Distance - stop.NodeDistance);
        if (along <= reach)
        {
            stations.push_back(station);
            continue;
        }
        if (!stations.empty())
        {
            const float32 previous = std::abs(stations.back().Distance - stop.NodeDistance);
            const float32 t = along > previous ? (reach - previous) / (along - previous) : 0.0f;
            stations.push_back(Interpolated(stations.back(), station, t));
        }
        break;
    }
    // Outward travel that falls in arc length has the rising travel's sides swapped.
    if (stations.size() >= 2u && stations.back().Distance < stations.front().Distance)
    {
        for (SG::PavedArmStation& station : stations)
            std::swap(station.HalfWidthLeft, station.HalfWidthRight);
    }
    return stations;
}

// The road's centerline in rising arc length, one point per arc length.
PavingRoadUV CenterlineOf(RoadArms arms)
{
    std::sort(arms.Stations.begin(), arms.Stations.end(),
              [](const SG::PavedArmStation& a, const SG::PavedArmStation& b) { return a.Distance < b.Distance; });
    PavingRoadUV road;
    for (const SG::PavedArmStation& station : arms.Stations)
    {
        if (!road.Distance.empty() && station.Distance - road.Distance.back() < kSameStationMeters)
            continue;
        road.Points.push_back(station.Position);
        road.Distance.push_back(station.Distance);
        road.HalfLeft.push_back(station.HalfWidthLeft);
        road.HalfRight.push_back(station.HalfWidthRight);
    }
    road.Spans = std::move(arms.Spans);
    return road;
}

// Whether road `a` claims the lid before road `b`.
bool ClaimsBefore(const RoadArms& a, const RoadArms& b)
{
    const bool aThrough = a.ArmCount >= 2u;
    const bool bThrough = b.ArmCount >= 2u;
    if (aThrough != bThrough)
        return aThrough;
    const float32 aWidth = a.CapHalfLeft + a.CapHalfRight;
    const float32 bWidth = b.CapHalfLeft + b.CapHalfRight;
    if (aWidth != bWidth)
        return aWidth > bWidth;
    return RanksBefore(a.Rank, b.Rank);
}

// The arms of region `region` grouped by road, each road with its keys from `roadRanks`. A road with no
// keys is left out.
std::vector<RoadArms> GatherRoadArms(const PavingPlan& plan, uint32 region, std::span<const PavingRank> roadRanks)
{
    std::vector<RoadArms> roads;
    for (uint32 a = 0; a < plan.Footprint.Arms.size(); ++a)
    {
        const SG::PavedArmStop& stop = plan.Footprint.Arms[a];
        if (stop.Region != region)
            continue;
        std::vector<SG::PavedArmStation> stations = StationsToStop(plan.Arms[a], stop);
        if (stations.size() < 2u)
            continue;
        const uint32 roadId = plan.ArmRoads[a];
        auto found = std::find_if(roads.begin(), roads.end(), [roadId](const RoadArms& road) { return road.Road == roadId; });
        if (found == roads.end())
        {
            const auto rank = std::find_if(roadRanks.begin(), roadRanks.end(),
                                           [roadId](const PavingRank& keys) { return keys.Entity == roadId; });
            if (rank == roadRanks.end())
                continue;
            found = roads.insert(roads.end(), RoadArms{});
            found->Road = roadId;
            found->Rank = *rank;
            found->CapHalfLeft = stations.back().HalfWidthLeft;
            found->CapHalfRight = stations.back().HalfWidthRight;
            found->RoadLoopMeters = plan.Arms[a].RoadLoopMeters;
        }
        const float32 node = stations.front().Distance;
        const float32 end = stations.back().Distance;
        found->Spans.emplace_back(std::min(node, end), std::max(node, end));
        found->Stations.insert(found->Stations.end(), stations.begin(), stations.end());
        ++found->ArmCount;
    }
    return roads;
}

} // namespace

std::vector<PavingRoadUV> RegionRoadUVs(const PavingPlan& plan, uint32 region,
                                        std::span<const PavingSplineObservation> splines,
                                        std::span<const PavingRank> roadRanks)
{
    std::vector<RoadArms> arms = GatherRoadArms(plan, region, roadRanks);
    std::sort(arms.begin(), arms.end(), ClaimsBefore);
    std::vector<PavingRoadUV> roads;
    for (RoadArms& road : arms)
    {
        const auto spline = std::find_if(splines.begin(), splines.end(), [&road](const PavingSplineObservation& observed)
                                         { return observed.Entity == road.Road; });
        if (spline == splines.end() || !spline->Strip)
            continue;
        const std::optional<PavingCapUV> cap = StripCapUV(
            *spline->Strip, spline->Spline.WorldMatrix, road.CapHalfLeft, road.CapHalfRight, road.RoadLoopMeters);
        if (!cap)
            continue;
        const uint32 entity = road.Road;
        PavingRoadUV uv = CenterlineOf(std::move(road));
        if (uv.Points.size() < 2u)
            continue;
        uv.Entity = entity;
        uv.TilesPerMeterU = cap->TilesPerMeterU;
        uv.TilesPerMeterV = cap->TilesPerMeterV;
        uv.LeftV = cap->LeftV;
        roads.push_back(std::move(uv));
    }
    return roads;
}

PavingRoadPoint LocateOnRoad(const PavingRoadUV& road, const Vector2& xz)
{
    PavingRoadPoint point;
    const size_t last = road.Points.size() - 1u;
    size_t nearest = 0;
    float32 nearestSquared = std::numeric_limits<float32>::max();
    for (size_t i = 0; i < last; ++i)
    {
        const Vector2 delta = road.Points[i + 1] - road.Points[i];
        if (Vector2::Dot(delta, delta) < kMinSegmentMeters * kMinSegmentMeters)
            continue;
        const Vector2 offset = xz - Mathematics::ClosestPointOnSegment(xz, road.Points[i], road.Points[i + 1]);
        const float32 squared = Vector2::Dot(offset, offset);
        if (squared < nearestSquared)
        {
            nearestSquared = squared;
            nearest = i;
        }
    }

    const Vector2 a = road.Points[nearest];
    const Vector2 delta = road.Points[nearest + 1] - a;
    const float32 length = delta.Length();
    if (!(length >= kMinSegmentMeters))
    {
        point.Inside = -std::numeric_limits<float32>::max();
        return point;
    }
    const Vector2 forward = delta / length;
    // Past the centerline's ends the end segments run on straight.
    float32 t = Vector2::Dot(xz - a, delta) / (length * length);
    if (nearest > 0)
        t = std::max(t, 0.0f);
    if (nearest + 1 < last)
        t = std::min(t, 1.0f);
    const float32 width = Math::Clamp01(t);
    const float32 distance = Math::Lerp(road.Distance[nearest], road.Distance[nearest + 1], t);
    const float32 halfLeft = Math::Lerp(road.HalfLeft[nearest], road.HalfLeft[nearest + 1], width);
    const float32 halfRight = Math::Lerp(road.HalfRight[nearest], road.HalfRight[nearest + 1], width);
    // Right of travel is +X when traveling +Z.
    const Vector2 right(forward.y, -forward.x);
    const float32 lateral = Vector2::Dot(xz - (a + delta * t), right);

    float32 along = -std::numeric_limits<float32>::max();
    for (const Vector2& span : road.Spans)
        along = std::max(along, std::min(distance - span.x, span.y - distance));
    point.UV = Vector2(distance * road.TilesPerMeterU, road.LeftV + (lateral + halfLeft) * road.TilesPerMeterV);
    point.Inside = std::min({halfLeft + lateral, halfRight - lateral, along});
    point.Forward = forward;
    return point;
}

} // namespace GameEngine::SplineRecipes
