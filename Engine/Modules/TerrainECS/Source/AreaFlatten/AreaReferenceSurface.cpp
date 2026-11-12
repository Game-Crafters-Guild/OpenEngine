#include "AreaFlatten/AreaReferenceSurface.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Transcendental.h"
#include "Mathematics/Vector2.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>

namespace GameEngine::TerrainECS
{
namespace
{

using Mathematics::Vector2;

// The most samples along either axis. A larger area widens its step instead: the fill is smooth, so
// a coarser raster of it loses nothing a flatten can show.
constexpr uint32 kMaxSamplesPerAxis = 512;
// The samples a raster axis holds past the footprint's extent: one margin sample on each side and
// the sample that closes the extent.
constexpr uint32 kRasterPadSamples = 3;
// The solve stops when no sample moves more than this in one sweep, meters.
constexpr float64 kSolveToleranceMeters = 1.0e-4;
// The solve's sweep cap, per sample along the larger axis: over-relaxation converges in a small
// multiple of that count.
constexpr uint32 kSweepsPerAxisSample = 20;
// The smallest fraction of a step at which a sample sees the rim. A sample closer than this takes
// the rim's height through a very large weight, not a division by zero.
constexpr float64 kMinRimFraction = 1.0e-3;
// Below this cross product of the step and a rim edge, square meters, the two run parallel.
constexpr float64 kParallelDenominator = 1.0e-12;
// The slack on the step's and the edge's parameters, so a step through a rim station meets the
// edges on both sides of it despite rounding.
constexpr float64 kCrossingSlack = 1.0e-6;

// The rim's height at a point blended toward the targets of the roads whose bands cover it, the
// same weighted average a flatten pool takes.
float32 BlendOpenings(std::span<const AreaOpening> openings, float32 worldX, float32 worldZ, float32 rimY)
{
    float32 weightSum = 0.0f;
    float32 targetSum = 0.0f;
    for (const AreaOpening& opening : openings)
    {
        RouteSample nearest{};
        nearest.Distance = std::numeric_limits<float32>::infinity();
        for (const RoutePolyline& run : opening.Runs)
        {
            const RouteSample sample = ClosestStationXZ(run, worldX, worldZ);
            if (sample.Distance < nearest.Distance)
                nearest = sample;
        }
        const float32 weight =
            ShapeFalloffWeight(nearest.DistanceFromEdge(), opening.Falloff, opening.FalloffInward) * opening.Weight;
        if (!(weight > 0.0f))
            continue;
        const float32 target = opening.UseVolumeHeight ? nearest.Height + opening.TargetHeight : opening.TargetHeight;
        weightSum += weight;
        targetSum += weight * target;
    }
    if (!(weightSum > 0.0f))
        return rimY;
    return rimY + (targetSum / weightSum - rimY) * std::min(weightSum, 1.0f);
}

// Where the step from `inside` to `outside` first crosses the rim: the fraction of the step and the
// rim's own height there. The rim polygon closes from its last point to its first.
bool FindRimCrossing(std::span<const Vector2> rimXZ, std::span<const float32> rimY, const Vector2& inside,
                     const Vector2& outside, float64& outFraction, float32& outRimY)
{
    const Vector2 step = outside - inside;
    bool found = false;
    outFraction = std::numeric_limits<float64>::max();
    for (std::size_t end = 0, start = rimXZ.size() - 1; end < rimXZ.size(); start = end++)
    {
        const Vector2& edgeStart = rimXZ[start];
        const Vector2 edge = rimXZ[end] - edgeStart;
        const float64 denominator = static_cast<float64>(step.x) * edge.y - static_cast<float64>(step.y) * edge.x;
        if (std::fabs(denominator) < kParallelDenominator)
            continue; // The step runs along the edge: the neighbor edges hold its ends.
        const Vector2 toEdge = edgeStart - inside;
        const float64 alongStep =
            (static_cast<float64>(toEdge.x) * edge.y - static_cast<float64>(toEdge.y) * edge.x) / denominator;
        const float64 alongEdge =
            (static_cast<float64>(toEdge.x) * step.y - static_cast<float64>(toEdge.y) * step.x) / denominator;
        if (alongStep < -kCrossingSlack || alongStep > 1.0 + kCrossingSlack || alongEdge < -kCrossingSlack ||
            alongEdge > 1.0 + kCrossingSlack || alongStep >= outFraction)
            continue;
        outFraction = Math::Clamp01(static_cast<float32>(alongStep));
        outRimY = Math::Lerp(rimY[start], rimY[end], Math::Clamp01(static_cast<float32>(alongEdge)));
        found = true;
    }
    return found;
}

// Where the rim crosses a row or a column of samples: the position along the line and the rim's
// height there.
struct RimCrossing
{
    float32 Position = 0.0f;
    float32 RimY = 0.0f;
};

enum class LineAxis : uint8
{
    Row,    // a line of constant Z; positions along X
    Column, // a line of constant X; positions along Z
};

// The rim's crossings of one line, sorted by position. An edge counts when it straddles the line
// as in Mathematics::PointInPolygon, so the two agree on which samples are inside.
void CrossingsOfLine(std::span<const Vector2> rimXZ, std::span<const float32> rimY, LineAxis axis, float32 line,
                     std::vector<RimCrossing>& out)
{
    out.clear();
    for (std::size_t end = 0, start = rimXZ.size() - 1; end < rimXZ.size(); start = end++)
    {
        const Vector2& a = rimXZ[end];
        const Vector2& b = rimXZ[start];
        const float32 aAcross = axis == LineAxis::Row ? a.y : a.x;
        const float32 bAcross = axis == LineAxis::Row ? b.y : b.x;
        if ((aAcross > line) == (bAcross > line))
            continue;
        const float32 aAlong = axis == LineAxis::Row ? a.x : a.y;
        const float32 bAlong = axis == LineAxis::Row ? b.x : b.y;
        const float32 fraction = (line - aAcross) / (bAcross - aAcross);
        out.push_back({(bAlong - aAlong) * (line - aAcross) / (bAcross - aAcross) + aAlong,
                       rimY[end] + fraction * (rimY[start] - rimY[end])});
    }
    std::sort(out.begin(), out.end(),
              [](const RimCrossing& left, const RimCrossing& right) { return left.Position < right.Position; });
}

// The rim height interpolated between two crossings of one line, at `position` along it.
float64 InterpolateBetween(const RimCrossing& low, const RimCrossing& high, float32 position)
{
    const float32 span = high.Position - low.Position;
    const float32 fraction = span > 0.0f ? (position - low.Position) / span : 0.0f;
    return static_cast<float64>(low.RimY) + static_cast<float64>(fraction) * (high.RimY - low.RimY);
}

// One interior sample's five-point stencil, with each side's arm cut short where the rim crosses it
// (the Shortley-Weller form): the sample's value is (Fixed + sum Coefficient[k] * neighbor[k]) /
// Diagonal, where a neighbor past the rim adds to Fixed instead.
struct InteriorStencil
{
    uint32 Index = 0;
    uint32 Neighbor[4] = {};
    float64 Coefficient[4] = {};
    float64 Fixed = 0.0;
    float64 Diagonal = 1.0;
};

} // namespace

uint64 AreaReferenceSurface::HashInputs(const AreaSurfaceInputs& inputs)
{
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    hash = Hashing::Fnv1a64Values(hash, inputs.Rim.Stations);
    hash = Hashing::Fnv1a64Value(hash, static_cast<uint64>(inputs.Openings.size()));
    for (const AreaOpening& opening : inputs.Openings)
    {
        hash = Hashing::Fnv1a64Value(hash, static_cast<uint64>(opening.Runs.size()));
        for (const RoutePolyline& run : opening.Runs)
            hash = Hashing::Fnv1a64Values(hash, run.Stations);
        hash = Hashing::Fnv1a64Value(hash, opening.TargetHeight);
        hash = Hashing::Fnv1a64Value(hash, static_cast<uint64>(opening.UseVolumeHeight ? 1u : 0u));
        hash = Hashing::Fnv1a64Value(hash, opening.Falloff);
        hash = Hashing::Fnv1a64Value(hash, opening.FalloffInward);
        hash = Hashing::Fnv1a64Value(hash, opening.Weight);
    }
    hash = Hashing::Fnv1a64Value(hash, inputs.MinX);
    hash = Hashing::Fnv1a64Value(hash, inputs.MinZ);
    hash = Hashing::Fnv1a64Value(hash, inputs.MaxX);
    hash = Hashing::Fnv1a64Value(hash, inputs.MaxZ);
    return Hashing::Fnv1a64Value(hash, inputs.Spacing);
}

void AreaReferenceSurface::Build(const AreaSurfaceInputs& inputs)
{
    assert(inputs.Rim.Stations.size() >= 4 && "a closed rim has at least three distinct stations");
    assert(inputs.MaxX > inputs.MinX && inputs.MaxZ > inputs.MinZ && inputs.Spacing > 0.0f);
    m_InputKey = HashInputs(inputs);

    // The raster: one sample of margin past the footprint on every side, so every sample inside the
    // rim has four neighbors.
    const float32 extentX = inputs.MaxX - inputs.MinX;
    const float32 extentZ = inputs.MaxZ - inputs.MinZ;
    m_Spacing = std::max(inputs.Spacing, std::max(extentX, extentZ) / static_cast<float32>(kMaxSamplesPerAxis - kRasterPadSamples));
    m_Width = static_cast<uint32>(std::ceil(extentX / m_Spacing)) + kRasterPadSamples;
    m_Height = static_cast<uint32>(std::ceil(extentZ / m_Spacing)) + kRasterPadSamples;
    m_OriginX = inputs.MinX - m_Spacing;
    m_OriginZ = inputs.MinZ - m_Spacing;
    const std::size_t sampleCount = static_cast<std::size_t>(m_Width) * m_Height;

    // The rim as a polygon, its closing station dropped.
    std::vector<Vector2> rimXZ;
    std::vector<float32> rimY;
    const std::size_t rimCount = inputs.Rim.Stations.size() - 1;
    rimXZ.reserve(rimCount);
    rimY.reserve(rimCount);
    for (std::size_t stationIndex = 0; stationIndex < rimCount; ++stationIndex)
    {
        const RouteStation& station = inputs.Rim.Stations[stationIndex];
        rimXZ.emplace_back(station.X, station.Z);
        rimY.push_back(station.Y);
    }

    // A first guess inside the rim: the mean of the rim heights interpolated along the sample's row
    // and along its column, near the harmonic fill for little cost. The row walk also decides which
    // samples lie inside the rim, with the even-odd rule of Mathematics::PointInPolygon.
    std::vector<float64> heights(sampleCount, 0.0);
    std::vector<uint8> inside(sampleCount, 0);
    std::vector<RimCrossing> crossings;
    for (uint32 row = 1; row + 1 < m_Height; ++row)
    {
        CrossingsOfLine(rimXZ, rimY, LineAxis::Row, m_OriginZ + static_cast<float32>(row) * m_Spacing, crossings);
        std::size_t passed = 0;
        for (uint32 column = 1; column + 1 < m_Width; ++column)
        {
            const float32 worldX = m_OriginX + static_cast<float32>(column) * m_Spacing;
            while (passed < crossings.size() && !(worldX < crossings[passed].Position))
                ++passed;
            if (passed == 0 || (crossings.size() - passed) % 2 == 0)
                continue;
            const std::size_t index = static_cast<std::size_t>(row) * m_Width + column;
            inside[index] = 1;
            heights[index] = InterpolateBetween(crossings[passed - 1], crossings[passed], worldX);
        }
    }
    for (uint32 column = 1; column + 1 < m_Width; ++column)
    {
        CrossingsOfLine(rimXZ, rimY, LineAxis::Column, m_OriginX + static_cast<float32>(column) * m_Spacing,
                        crossings);
        std::size_t passed = 0;
        for (uint32 row = 1; row + 1 < m_Height; ++row)
        {
            const float32 worldZ = m_OriginZ + static_cast<float32>(row) * m_Spacing;
            while (passed < crossings.size() && !(worldZ < crossings[passed].Position))
                ++passed;
            const std::size_t index = static_cast<std::size_t>(row) * m_Width + column;
            if (!inside[index] || passed == 0 || (crossings.size() - passed) % 2 == 0)
                continue;
            heights[index] = 0.5 * (heights[index] + InterpolateBetween(crossings[passed - 1], crossings[passed], worldZ));
        }
    }

    // Outside the rim the surface is the nearest rim height, blended toward the roads.
    for (uint32 row = 0; row < m_Height; ++row)
    {
        const float32 worldZ = m_OriginZ + static_cast<float32>(row) * m_Spacing;
        for (uint32 column = 0; column < m_Width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * m_Width + column;
            if (inside[index])
                continue;
            const float32 worldX = m_OriginX + static_cast<float32>(column) * m_Spacing;
            const float32 rimHeight = ClosestStationXZ(inputs.Rim, worldX, worldZ).Height;
            heights[index] = BlendOpenings(inputs.Openings, worldX, worldZ, rimHeight);
        }
    }

    // The stencils. Order: +X, -X, +Z, -Z.
    constexpr int32 kStepX[4] = {1, -1, 0, 0};
    constexpr int32 kStepZ[4] = {0, 0, 1, -1};
    std::vector<InteriorStencil> stencils;
    for (uint32 row = 1; row + 1 < m_Height; ++row)
    {
        for (uint32 column = 1; column + 1 < m_Width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * m_Width + column;
            if (!inside[index])
                continue;
            const Vector2 here(m_OriginX + static_cast<float32>(column) * m_Spacing,
                               m_OriginZ + static_cast<float32>(row) * m_Spacing);
            InteriorStencil stencil{};
            stencil.Index = static_cast<uint32>(index);
            float64 arm[4] = {1.0, 1.0, 1.0, 1.0};
            float64 rimValue[4] = {};
            bool cut[4] = {};
            for (uint32 side = 0; side < 4; ++side)
            {
                const uint32 neighborColumn = static_cast<uint32>(static_cast<int32>(column) + kStepX[side]);
                const uint32 neighborRow = static_cast<uint32>(static_cast<int32>(row) + kStepZ[side]);
                const std::size_t neighbor = static_cast<std::size_t>(neighborRow) * m_Width + neighborColumn;
                stencil.Neighbor[side] = static_cast<uint32>(neighbor);
                if (inside[neighbor])
                    continue;
                const Vector2 there(m_OriginX + static_cast<float32>(neighborColumn) * m_Spacing,
                                    m_OriginZ + static_cast<float32>(neighborRow) * m_Spacing);
                float64 fraction = 1.0;
                float32 crossingRimY = 0.0f;
                cut[side] = true;
                if (FindRimCrossing(rimXZ, rimY, here, there, fraction, crossingRimY))
                {
                    arm[side] = std::max(fraction, kMinRimFraction);
                    const Vector2 crossing = Mathematics::Lerp(here, there, static_cast<float32>(fraction));
                    rimValue[side] = BlendOpenings(inputs.Openings, crossing.x, crossing.y, crossingRimY);
                }
                else
                {
                    rimValue[side] = heights[neighbor];
                }
            }
            const float64 scaleX = 2.0 / (arm[0] + arm[1]);
            const float64 scaleZ = 2.0 / (arm[2] + arm[3]);
            stencil.Diagonal = 0.0;
            for (uint32 side = 0; side < 4; ++side)
            {
                const float64 coefficient = (side < 2 ? scaleX : scaleZ) / arm[side];
                stencil.Diagonal += coefficient;
                if (cut[side])
                    stencil.Fixed += coefficient * rimValue[side];
                else
                    stencil.Coefficient[side] = coefficient;
            }
            stencils.push_back(stencil);
        }
    }

    // Successive over-relaxation in a fixed order, with the factor that is optimal for the square
    // that holds the raster.
    const uint32 longest = std::max(m_Width, m_Height);
    const float64 relaxation = 2.0 / (1.0 + Math::Sin(std::numbers::pi / static_cast<float64>(longest)));
    const uint32 sweepCap = kSweepsPerAxisSample * longest;
    for (uint32 sweep = 0; sweep < sweepCap; ++sweep)
    {
        float64 largestMove = 0.0;
        for (const InteriorStencil& stencil : stencils)
        {
            float64 sum = stencil.Fixed;
            for (uint32 side = 0; side < 4; ++side)
                sum += stencil.Coefficient[side] * heights[stencil.Neighbor[side]];
            float64& value = heights[stencil.Index];
            const float64 move = relaxation * (sum / stencil.Diagonal - value);
            value += move;
            largestMove = std::max(largestMove, std::fabs(move));
        }
        if (largestMove < kSolveToleranceMeters)
            break;
    }

    m_Heights.resize(sampleCount);
    for (std::size_t i = 0; i < sampleCount; ++i)
        m_Heights[i] = static_cast<float32>(heights[i]);
}

float32 AreaReferenceSurface::Sample(float32 worldX, float32 worldZ) const
{
    assert(!m_Heights.empty() && "the surface is sampled after it is built");
    const float32 maxColumn = static_cast<float32>(m_Width - 1);
    const float32 maxRow = static_cast<float32>(m_Height - 1);
    const float32 u = std::clamp((worldX - m_OriginX) / m_Spacing, 0.0f, maxColumn);
    const float32 v = std::clamp((worldZ - m_OriginZ) / m_Spacing, 0.0f, maxRow);
    const uint32 column = std::min(static_cast<uint32>(u), m_Width - 2);
    const uint32 row = std::min(static_cast<uint32>(v), m_Height - 2);
    const float32 tx = u - static_cast<float32>(column);
    const float32 tz = v - static_cast<float32>(row);
    const float32* low = &m_Heights[static_cast<std::size_t>(row) * m_Width + column];
    const float32* high = low + m_Width;
    return Math::Lerp(Math::Lerp(low[0], low[1], tx), Math::Lerp(high[0], high[1], tx), tz);
}

} // namespace GameEngine::TerrainECS
