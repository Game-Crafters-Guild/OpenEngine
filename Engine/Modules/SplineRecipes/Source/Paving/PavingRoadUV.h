#pragma once

#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Components
{
struct SplineExtrude;
}

namespace GameEngine::Mathematics
{
struct Matrix4x4;
}

namespace GameEngine::SplineRecipes
{

struct PavingPlan;
struct PavingRank;
struct PavingSplineObservation;

// The UV the strip of a road gives the flat top of a square cap at a paved junction. U is the road's
// arc length times TilesPerMeterU; V runs left to right of rising arc length, LeftV at the top's left
// end plus TilesPerMeterV per meter across.
struct PavingCapUV
{
    float32 TilesPerMeterU = 1.0f;
    float32 TilesPerMeterV = 1.0f;
    float32 LeftV = 0.0f;
};

// A road's strip mapping laid over a paved region it enters, so the lid continues the road's grid.
struct PavingRoadUV
{
    // The road's entity id.
    uint32 Entity = 0;
    // In rising arc length: the centerline on the ground plane (x, z), the road's arc length at each
    // point in world meters, and the top's half-widths left and right of rising travel.
    std::vector<Mathematics::Vector2> Points;
    std::vector<float32> Distance;
    std::vector<float32> HalfLeft;
    std::vector<float32> HalfRight;
    // The arc lengths the region takes over, one span per arm: x low, y high.
    std::vector<Mathematics::Vector2> Spans;
    // StripCapUV of the road's strip.
    float32 TilesPerMeterU = 1.0f;
    float32 TilesPerMeterV = 1.0f;
    float32 LeftV = 0.0f;
};

// A ground-plane point read against one road.
struct PavingRoadPoint
{
    // The UV the road's strip mapping gives the point, extended past the strip's top.
    Mathematics::Vector2 UV{};
    // Meters inside the road's ribbon (its top over the spans), negative outside.
    float32 Inside = 0.0f;
    // The direction of rising arc length at the point, unit length.
    Mathematics::Vector2 Forward{};
};

// The roads that enter region `region` of `plan` and sweep a strip, in the order they claim the lid:
// a road with two arms in the region (it runs through) first, then the wider top, then the first of
// `roadRanks` under RanksBefore. A road with no entry in `splines` or `roadRanks` claims nothing.
std::vector<PavingRoadUV> RegionRoadUVs(const PavingPlan& plan, uint32 region,
                                        std::span<const PavingSplineObservation> splines,
                                        std::span<const PavingRank> roadRanks);

// `xz` read against `road`: its nearest centerline segment, the end segments extended past the ends.
PavingRoadPoint LocateOnRoad(const PavingRoadUV& road, const Mathematics::Vector2& xz);

// The cap UV of the strip of `recipe` where its top spans `topHalfLeft` and `topHalfRight` world meters,
// swept under `worldMatrix`, `roadLoopMeters` long when closed (0 when open). None for a water fill.
// SplineExtrudeController.cpp defines it beside the sweep whose UV it repeats.
std::optional<PavingCapUV> StripCapUV(const Components::SplineExtrude& recipe,
                                      const Mathematics::Matrix4x4& worldMatrix, float32 topHalfLeft,
                                      float32 topHalfRight, float32 roadLoopMeters);

} // namespace GameEngine::SplineRecipes
