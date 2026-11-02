#include "SplineRecipes/Paving/PavingPlan.h"

#include "Mathematics/Geometry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;

// The network's ordering resolutions: lengths closer than a centimeter, and points closer than
// its touching tolerance, are equal.
constexpr float32 kRankLengthResolutionMeters = 0.01f;
constexpr float32 kRankPointResolutionMeters = 1.0e-3f;

// Two arms of one road leave one junction when their node arc lengths are this close, in meters.
constexpr float32 kSameArmMeters = 0.01f;

// The station at `arcLength` along the road (wrapped on a closed road), interpolated between
// the samples either side.
SG::PavedArmStation StationAt(const PavingRoadCurve& road, float32 arcLength)
{
    if (road.Closed && road.Length > 0.0f)
    {
        arcLength = std::fmod(arcLength, road.Length);
        if (arcLength < 0.0f)
            arcLength += road.Length;
    }
    arcLength = std::clamp(arcLength, road.ArcLengths.front(), road.ArcLengths.back());
    const auto upper = std::lower_bound(road.ArcLengths.begin(), road.ArcLengths.end(), arcLength);
    const size_t high = std::clamp<size_t>(static_cast<size_t>(upper - road.ArcLengths.begin()), 1u,
                                           road.ArcLengths.size() - 1u);
    const size_t low = high - 1u;
    const float32 span = road.ArcLengths[high] - road.ArcLengths[low];
    const float32 t = span > 0.0f ? (arcLength - road.ArcLengths[low]) / span : 0.0f;
    const Mathematics::Vector3 point = road.Points[low] + (road.Points[high] - road.Points[low]) * t;
    const float32 halfWidth = road.HalfWidths[low] + (road.HalfWidths[high] - road.HalfWidths[low]) * t;
    SG::PavedArmStation station;
    station.Position = Vector2(point.x, point.z);
    station.HalfWidthLeft = halfWidth;
    station.HalfWidthRight = halfWidth;
    station.Distance = arcLength;
    return station;
}

bool SameArm(const SG::PavedArm& a, const SG::PavedArm& b)
{
    if (a.Road != b.Road || a.Stations.size() < 2u || b.Stations.size() < 2u)
        return false;
    const bool aRises = a.Stations[1].Distance > a.Stations[0].Distance;
    const bool bRises = b.Stations[1].Distance > b.Stations[0].Distance;
    return aRises == bRises && std::fabs(a.Stations[0].Distance - b.Stations[0].Distance) <= kSameArmMeters;
}

// The source in `candidates` that ranks first.
uint32 FirstByRank(std::span<const PavingSource> sources, std::span<const uint32> candidates)
{
    uint32 best = candidates.front();
    for (const uint32 candidate : candidates)
        if (RanksBefore(sources[candidate].Rank, sources[best].Rank))
            best = candidate;
    return best;
}

bool Contains(std::span<const uint32> values, uint32 value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Whether `point` lies inside any of `bounds`.
bool InsideAny(const Vector2& point, std::span<const SG::AreaRing> bounds)
{
    return std::any_of(bounds.begin(), bounds.end(),
                       [&point](const SG::AreaRing& ring) { return Mathematics::PointInPolygon(point, ring); });
}

// Whether station `stationIndex` of `arm` and both edges of its strip, `borderWidth` wider, lie outside `bounds`.
bool StripLeavesBounds(const SG::PavedArm& arm, size_t stationIndex, std::span<const SG::AreaRing> bounds,
                       float32 borderWidth)
{
    const SG::PavedArmStation& station = arm.Stations[stationIndex];
    if (InsideAny(station.Position, bounds))
        return false;
    const Vector2 chord = station.Position - arm.Stations[stationIndex - 1u].Position;
    if (!(chord.Length() > 0.0f))
        return true;
    const Vector2 along = chord.Normalize();
    const Vector2 left(-along.y, along.x);
    return !InsideAny(station.Position + left * (station.HalfWidthLeft + borderWidth), bounds) &&
           !InsideAny(station.Position - left * (station.HalfWidthRight + borderWidth), bounds);
}

// The rings an unpaved road of `source` is cut out of: its outline, or on a Road the ribbons of its
// paved arms, the border's width wider on each side.
std::vector<SG::AreaRing> CutBounds(const PavingSource& source)
{
    if (!source.Outline.empty())
        return {source.Outline};
    std::vector<SG::PavedArm> paved;
    for (const SG::PavedArm& arm : source.Arms)
    {
        if (Contains(source.UnpavedRoads, arm.Road))
            continue;
        SG::PavedArm& wider = paved.emplace_back(arm);
        for (SG::PavedArmStation& station : wider.Stations)
        {
            station.HalfWidthLeft += source.BorderWidth;
            station.HalfWidthRight += source.BorderWidth;
        }
    }
    std::vector<SG::AreaRing> ribbons = SG::BuildPavedArmRibbons(paved);
    std::erase_if(ribbons, [](const SG::AreaRing& ribbon) { return ribbon.empty(); });
    return ribbons;
}

// The part of an unpaved road's arm that `bounds` cut: its stations to the first one whose strip,
// `borderWidth` wider on each side, lies wholly outside them. The cut is `margin` wider on each side
// than the strip, and `margin` longer where the arm ends its road.
SG::PavedArm CutArm(const SG::PavedArm& arm, std::span<const SG::AreaRing> bounds, float32 borderWidth,
                    float32 margin)
{
    SG::PavedArm cut = arm;
    for (size_t i = 1; i < cut.Stations.size(); ++i)
    {
        if (!StripLeavesBounds(cut, i, bounds, borderWidth))
            continue;
        cut.Stations.resize(i + 1u);
        cut.EndsRoad = false;
        break;
    }
    for (SG::PavedArmStation& station : cut.Stations)
    {
        station.HalfWidthLeft += margin;
        station.HalfWidthRight += margin;
    }
    if (cut.EndsRoad && margin > 0.0f && cut.Stations.size() >= 2u)
    {
        const SG::PavedArmStation& last = cut.Stations.back();
        const SG::PavedArmStation& previous = cut.Stations[cut.Stations.size() - 2u];
        const Vector2 chord = last.Position - previous.Position;
        const float32 length = chord.Length();
        if (length > 0.0f)
        {
            SG::PavedArmStation past = last;
            past.Position = last.Position + chord * (margin / length);
            past.Distance = last.Distance + (last.Distance >= previous.Distance ? margin : -margin);
            cut.Stations.push_back(past);
        }
    }
    return cut;
}

} // namespace

SG::PavedArm BuildPavedArm(const PavingRoadCurve& road, float32 nodeArcLength, int8 direction, const Vector2& node)
{
    SG::PavedArm arm;
    arm.Road = road.Road.id;
    arm.Node = node;
    if (road.Points.size() < 2u || road.Length <= 0.0f)
        return arm;
    const float32 step = direction >= 0 ? 1.0f : -1.0f;
    float32 reach = kPavedArmReachMeters;
    if (road.Closed)
    {
        arm.RoadLoopMeters = road.Length;
        reach = std::min(reach, 0.5f * road.Length);
    }
    else
    {
        const float32 toEnd = direction >= 0 ? road.Length - nodeArcLength : nodeArcLength;
        arm.EndsRoad = toEnd <= reach;
        reach = std::clamp(toEnd, 0.0f, reach);
    }

    // The samples strictly between the node and the far end, by distance outward; a closed road's
    // samples are taken on the laps either side as well, so the arm can cross the seam.
    std::vector<float32> outward;
    for (const float32 lap : {-road.Length, 0.0f, road.Length})
    {
        if (!road.Closed && lap != 0.0f)
            continue;
        for (const float32 arc : road.ArcLengths)
        {
            const float32 along = (arc + lap - nodeArcLength) * step;
            if (along > 0.0f && along < reach)
                outward.push_back(along);
        }
    }
    std::sort(outward.begin(), outward.end());
    // A closed road's last sample repeats its first one lap on.
    outward.erase(std::unique(outward.begin(), outward.end(),
                              [](float32 a, float32 b) { return b - a < SG::kPavedTouchMeters; }),
                  outward.end());

    arm.Stations.push_back(StationAt(road, nodeArcLength));
    for (const float32 along : outward)
        arm.Stations.push_back(StationAt(road, nodeArcLength + step * along));
    arm.Stations.push_back(StationAt(road, nodeArcLength + step * reach));
    return arm;
}

bool RanksBefore(const PavingRank& a, const PavingRank& b)
{
    if (std::fabs(a.LengthMeters - b.LengthMeters) > kRankLengthResolutionMeters)
        return a.LengthMeters > b.LengthMeters;
    if (std::fabs(a.FirstPoint.x - b.FirstPoint.x) > kRankPointResolutionMeters)
        return a.FirstPoint.x < b.FirstPoint.x;
    if (std::fabs(a.FirstPoint.y - b.FirstPoint.y) > kRankPointResolutionMeters)
        return a.FirstPoint.y < b.FirstPoint.y;
    if (!a.SceneId.empty() && !b.SceneId.empty() && a.SceneId != b.SceneId)
        return a.SceneId < b.SceneId;
    return a.Entity < b.Entity;
}

PavingPlan PlanPavings(std::span<const PavingSource> sources)
{
    PavingPlan plan;
    std::vector<SG::AreaRing> outlines;
    std::vector<float32> mouthRadii;
    std::vector<uint32> outlineSources;
    std::vector<SG::PavedArm> arms;
    // Every source that brought each arm: an arm two sources share is listed under both.
    std::vector<std::vector<uint32>> armSources;
    std::vector<SG::AreaRing> excludes;
    // An outline's unpaved roads are cut with the curb beside them; a Road's are cut flush, since
    // the left-out road's surface meets the paving road's at one height.
    std::vector<SG::PavedArm> cutArms;
    std::vector<SG::PavedArm> flushCutArms;
    plan.SourceArms.resize(sources.size());
    for (uint32 s = 0; s < sources.size(); ++s)
    {
        const PavingSource& source = sources[s];
        if (!source.Outline.empty())
        {
            outlines.push_back(source.Outline);
            mouthRadii.push_back(source.MouthRadius);
            outlineSources.push_back(s);
        }
        excludes.insert(excludes.end(), source.Excludes.begin(), source.Excludes.end());
        std::vector<SG::AreaRing> cutBounds;
        for (const SG::PavedArm& arm : source.Arms)
        {
            if (Contains(source.UnpavedRoads, arm.Road))
            {
                if (cutBounds.empty())
                    cutBounds = CutBounds(source);
                if (!cutBounds.empty() && !source.Outline.empty())
                    cutArms.push_back(CutArm(arm, cutBounds, source.BorderWidth, source.BorderWidth));
                else if (!cutBounds.empty())
                    flushCutArms.push_back(CutArm(arm, cutBounds, source.BorderWidth, 0.0f));
                plan.SourceArms[s].push_back(kNoPavedArm);
                continue;
            }
            const auto same = std::find_if(arms.begin(), arms.end(),
                                           [&arm](const SG::PavedArm& kept) { return SameArm(kept, arm); });
            if (same != arms.end())
            {
                armSources[static_cast<size_t>(same - arms.begin())].push_back(s);
                plan.SourceArms[s].push_back(static_cast<uint32>(same - arms.begin()));
                continue;
            }
            plan.SourceArms[s].push_back(static_cast<uint32>(arms.size()));
            arms.push_back(arm);
            armSources.push_back({s});
        }
    }

    for (SG::AreaRing& ribbon : SG::BuildPavedArmRibbons(cutArms))
    {
        if (!ribbon.empty())
            excludes.push_back(std::move(ribbon));
    }
    std::vector<SG::AreaRing> flushCuts = SG::BuildPavedArmRibbons(flushCutArms);
    std::erase_if(flushCuts, [](const SG::AreaRing& ribbon) { return ribbon.empty(); });

    SG::PavedFootprintInput input;
    input.Outlines = outlines;
    input.Arms = arms;
    input.Excludes = excludes;
    input.FlushCuts = flushCuts;
    input.MouthRadii = mouthRadii;
    plan.Footprint = SG::BuildPavedFootprint(input);
    for (const SG::PavedArm& arm : arms)
        plan.ArmRoads.push_back(arm.Road);
    plan.Arms = std::move(arms);
    plan.Outlines = std::move(outlines);

    // The sources each region holds: its outlines' and its arms'.
    std::vector<std::vector<uint32>> regionSources(plan.Footprint.Regions.size());
    plan.RegionOwners.resize(plan.Footprint.Regions.size(), 0u);
    for (uint32 r = 0; r < plan.Footprint.Regions.size(); ++r)
    {
        const SG::PavedRegion& region = plan.Footprint.Regions[r];
        std::vector<uint32> outlineOwners;
        for (const uint32 outline : region.Outlines)
            outlineOwners.push_back(outlineSources[outline]);
        regionSources[r] = outlineOwners;
        for (const uint32 arm : region.Arms)
            regionSources[r].insert(regionSources[r].end(), armSources[arm].begin(), armSources[arm].end());
        if (!outlineOwners.empty())
            plan.RegionOwners[r] = FirstByRank(sources, outlineOwners);
        else if (!regionSources[r].empty())
            plan.RegionOwners[r] = FirstByRank(sources, regionSources[r]);
    }

    plan.Roles.resize(sources.size(), PavingRole::NothingToPave);
    plan.PavedBy.resize(sources.size(), 0u);
    for (uint32 s = 0; s < sources.size(); ++s)
    {
        if (Contains(plan.RegionOwners, s))
        {
            plan.Roles[s] = PavingRole::Owns;
            continue;
        }
        for (uint32 r = 0; r < regionSources.size(); ++r)
        {
            if (!Contains(regionSources[r], s))
                continue;
            plan.Roles[s] = PavingRole::PavedBy;
            plan.PavedBy[s] = plan.RegionOwners[r];
            break;
        }
    }
    return plan;
}

} // namespace GameEngine::SplineRecipes
