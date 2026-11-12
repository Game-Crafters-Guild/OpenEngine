#include "TerrainECS/TerrainRoutePolyline.h"

#include "Mathematics/Interpolation.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

namespace GameEngine::TerrainECS
{

namespace
{

constexpr float32 kDegenerateSegmentLengthSq = 1e-9f;

// Cells per segment at most: a route that fills a square gets about four cells
// per segment, a straight route about half a cell.
constexpr uint32 kMaxCellsPerSegment = 4u;
// The cell is at least this many mean segment lengths wide, so one cell holds a
// few consecutive segments rather than splitting each one across cells.
constexpr float32 kMinCellSegmentLengths = 2.0f;
// The cell is at least this fraction of the query reach (the widest half-width
// plus the falloff), so a query in the band visits about two rings of cells.
constexpr float32 kMinCellReachFraction = 0.5f;
// Rounding slack: a fraction of the cell, plus a relative term for the float
// spacing at large world coordinates.
constexpr float32 kSlackCellFraction = 0.01f;
constexpr float32 kSlackRelative = 1e-5f;

struct SegmentHit
{
    float32 Distance = 0.0f;
    float32 T = 0.0f;
};

// The XZ distance to segment [a, b] and the clamped parameter of the nearest
// point; false for coincident stations. The scan and the grid share it, so both
// compute the same floats for the same segment.
bool HitSegment(const RouteStation& a, const RouteStation& b, float32 worldX, float32 worldZ,
                SegmentHit& out)
{
    const float32 dx = b.X - a.X;
    const float32 dz = b.Z - a.Z;
    const float32 lengthSq = dx * dx + dz * dz;
    if (lengthSq < kDegenerateSegmentLengthSq)
        return false;

    out.T = Math::Clamp01(((worldX - a.X) * dx + (worldZ - a.Z) * dz) / lengthSq);
    const float32 offX = worldX - (a.X + out.T * dx);
    const float32 offZ = worldZ - (a.Z + out.T * dz);
    out.Distance = std::sqrt(offX * offX + offZ * offZ);
    return true;
}

RouteSample SampleOnSegment(const RoutePolyline& polyline, uint32 segment, const SegmentHit& hit)
{
    const RouteStation& a = polyline.Stations[segment];
    const RouteStation& b = polyline.Stations[segment + 1u];
    RouteSample sample{};
    sample.Distance = hit.Distance;
    sample.Height = a.Y + hit.T * (b.Y - a.Y);
    sample.HalfWidth = a.HalfWidth + hit.T * (b.HalfWidth - a.HalfWidth);
    return sample;
}

// The cell that holds `value` along an axis, clamped to the grid. Clamped in
// float first, so a far query never overflows the integer.
uint32 CellAlong(float32 value, float32 origin, float32 cellSize, uint32 cells)
{
    const float32 cell = std::floor((value - origin) / cellSize);
    return static_cast<uint32>(std::clamp(cell, 0.0f, static_cast<float32>(cells - 1u)));
}

struct CellRange
{
    uint32 X0 = 0, X1 = 0, Z0 = 0, Z1 = 0;
};

// The cells the padded XZ box of `segment` meets. False for a degenerate segment,
// which the grid leaves out as the scan skips it.
bool SegmentCells(const RoutePolyline& polyline, const RouteSegmentGrid& grid, uint32 segment, CellRange& out)
{
    const RouteStation& a = polyline.Stations[segment];
    const RouteStation& b = polyline.Stations[segment + 1u];
    const float32 dx = b.X - a.X;
    const float32 dz = b.Z - a.Z;
    if (dx * dx + dz * dz < kDegenerateSegmentLengthSq)
        return false;
    out.X0 = CellAlong(std::min(a.X, b.X) - grid.Slack, grid.OriginX, grid.CellSize, grid.CellsX);
    out.X1 = CellAlong(std::max(a.X, b.X) + grid.Slack, grid.OriginX, grid.CellSize, grid.CellsX);
    out.Z0 = CellAlong(std::min(a.Z, b.Z) - grid.Slack, grid.OriginZ, grid.CellSize, grid.CellsZ);
    out.Z1 = CellAlong(std::max(a.Z, b.Z) + grid.Slack, grid.OriginZ, grid.CellSize, grid.CellsZ);
    return true;
}

// Tests every segment of one cell against the best so far: nearer wins, and an
// equally near segment wins only with a lower index, as in the scan.
void TestCell(const RoutePolyline& polyline, const RouteSegmentGrid& grid, std::size_t cell, float32 worldX,
              float32 worldZ, uint32& best, SegmentHit& bestHit)
{
    for (uint32 k = grid.CellStart[cell]; k < grid.CellStart[cell + 1u]; ++k)
    {
        const uint32 i = grid.Segments[k];
        SegmentHit hit;
        if (HitSegment(polyline.Stations[i], polyline.Stations[i + 1u], worldX, worldZ, hit) &&
            (hit.Distance < bestHit.Distance || (hit.Distance == bestHit.Distance && i < best)))
        {
            best = i;
            bestHit = hit;
        }
    }
}

uint32 CellsAlong(float32 extent, float32 cellSize)
{
    return static_cast<uint32>(std::floor(extent / cellSize)) + 1u;
}

RouteSample ScanClosestStation(const RoutePolyline& polyline, float32 worldX, float32 worldZ)
{
    RouteSample sample{};
    sample.Distance = std::numeric_limits<float32>::infinity();

    if (!polyline.IsValid())
        return sample;

    // Strictly nearer, so the FIRST winning segment keeps a tie. A sample
    // equidistant from two adjacent segments sits on their shared station,
    // where both interpolate to the same grade and width anyway.
    const uint32 segments = static_cast<uint32>(polyline.Stations.size() - 1u);
    for (uint32 i = 0; i < segments; ++i)
    {
        SegmentHit hit;
        if (HitSegment(polyline.Stations[i], polyline.Stations[i + 1u], worldX, worldZ, hit) &&
            hit.Distance < sample.Distance)
            sample = SampleOnSegment(polyline, i, hit);
    }

    return sample;
}

void BuildSegmentGrid(const RoutePolyline& polyline, float32 falloff, RouteSegmentGrid& out)
{
    out = RouteSegmentGrid{};
    if (!polyline.IsValid())
        return;

    const std::vector<RouteStation>& stations = polyline.Stations;
    const uint32 segments = static_cast<uint32>(stations.size() - 1u);

    float32 minX = stations[0].X, maxX = stations[0].X;
    float32 minZ = stations[0].Z, maxZ = stations[0].Z;
    float32 halfWidth = stations[0].HalfWidth;
    float32 length = 0.0f;
    for (uint32 i = 0; i < segments; ++i)
    {
        const RouteStation& b = stations[i + 1u];
        minX = std::min(minX, b.X);
        maxX = std::max(maxX, b.X);
        minZ = std::min(minZ, b.Z);
        maxZ = std::max(maxZ, b.Z);
        halfWidth = std::max(halfWidth, b.HalfWidth);
        length += std::hypot(b.X - stations[i].X, b.Z - stations[i].Z);
    }

    out.SegmentCount = segments;
    const float32 extentX = maxX - minX;
    const float32 extentZ = maxZ - minZ;
    const float32 reach = std::max(halfWidth, 0.0f) + std::max(falloff, 0.0f);
    if (!std::isfinite(extentX) || !std::isfinite(extentZ) || !std::isfinite(length) || !std::isfinite(reach))
        return; // No grid: every query takes the scan.

    const float32 maxCells = static_cast<float32>(kMaxCellsPerSegment) * static_cast<float32>(segments);
    float32 cellSize = std::max({kMinCellSegmentLengths * length / static_cast<float32>(segments),
                                 kMinCellReachFraction * reach,
                                 std::sqrt(extentX * extentZ / maxCells), extentX / maxCells,
                                 extentZ / maxCells, kMinRouteStationSpacing});
    while (static_cast<float32>(CellsAlong(extentX, cellSize)) * static_cast<float32>(CellsAlong(extentZ, cellSize)) >
           maxCells + 1.0f)
        cellSize *= 2.0f;

    const float32 magnitude = std::max({std::abs(minX), std::abs(maxX), std::abs(minZ), std::abs(maxZ)});
    out.OriginX = minX;
    out.OriginZ = minZ;
    out.CellSize = cellSize;
    out.Slack = kSlackCellFraction * cellSize + kSlackRelative * magnitude;
    out.CellsX = CellsAlong(extentX, cellSize);
    out.CellsZ = CellsAlong(extentZ, cellSize);
    out.CellStart.assign(static_cast<std::size_t>(out.CellsX) * out.CellsZ + 1u, 0u);

    // Two passes over the segments in index order (count, then fill), so each
    // cell's list is ascending.
    CellRange range;
    for (uint32 i = 0; i < segments; ++i)
        if (SegmentCells(polyline, out, i, range))
            for (uint32 z = range.Z0; z <= range.Z1; ++z)
                for (uint32 x = range.X0; x <= range.X1; ++x)
                    ++out.CellStart[static_cast<std::size_t>(z) * out.CellsX + x + 1u];
    for (std::size_t c = 1; c < out.CellStart.size(); ++c)
        out.CellStart[c] += out.CellStart[c - 1u];
    out.Segments.resize(out.CellStart.back());
    std::vector<uint32> cursor(out.CellStart.begin(), out.CellStart.end() - 1);
    for (uint32 i = 0; i < segments; ++i)
        if (SegmentCells(polyline, out, i, range))
            for (uint32 z = range.Z0; z <= range.Z1; ++z)
                for (uint32 x = range.X0; x <= range.X1; ++x)
                    out.Segments[cursor[static_cast<std::size_t>(z) * out.CellsX + x]++] = i;
}

} // namespace

void BuildRoutePolyline(const Spline::SplineData& spline, float32 stationSpacing, float32 falloff,
                        RoutePolyline& out)
{
    out.Stations.clear();
    out.SegmentGrid = RouteSegmentGrid{};

    const float32 totalLength = spline.TotalArcLength;
    if (!spline.IsValid() || spline.ArcLengthTable.empty() || !(totalLength > 0.0f))
        return;

    const float32 spacing = std::max(stationSpacing, kMinRouteStationSpacing);

    // Interval count from the route length, then the cap. Capping the COUNT
    // rather than clamping the spacing keeps a legitimately long route from
    // silently changing shape: it widens that one route's spacing instead.
    uint32 intervals = static_cast<uint32>(std::ceil(totalLength / spacing));
    intervals = std::clamp(intervals, 1u, kMaxRouteStations - 1u);

    out.Stations.reserve(static_cast<std::size_t>(intervals) + 1u);
    for (uint32 i = 0; i <= intervals; ++i)
    {
        // The last station is pinned to the route end rather than to i*spacing,
        // so a length that is not a whole multiple of the spacing still grades
        // all the way to its end point.
        const float32 distance = (i == intervals)
            ? totalLength
            : static_cast<float32>(i) * (totalLength / static_cast<float32>(intervals));

        const Spline::SplineFrame frame = Spline::EvaluateAtDistance(spline, distance);
        RouteStation station{};
        station.X = frame.Position.x;
        station.Z = frame.Position.z;
        station.Y = frame.Position.y;
        station.HalfWidth = frame.Radius;
        out.Stations.push_back(station);
    }
    BuildSegmentGrid(out, falloff, out.SegmentGrid);
}

void BuildRoutePolyline(std::vector<RouteStation> stations, float32 falloff, RoutePolyline& out)
{
    out.Stations = std::move(stations);
    BuildSegmentGrid(out, falloff, out.SegmentGrid);
}

RouteSample ClosestStationXZ(const RoutePolyline& polyline, float32 worldX, float32 worldZ)
{
    // With no grid, the scan answers. A grid for another station count means that Stations
    // changed after the build: the scan answers, and a build with asserts stops here.
    const RouteSegmentGrid& grid = polyline.SegmentGrid;
    assert((grid.CellStart.empty() || grid.SegmentCount + 1u == polyline.Stations.size()) &&
           "Stations changed after BuildRoutePolyline: call BuildRoutePolyline again");
    if (!polyline.IsValid() || grid.CellStart.empty() || grid.SegmentCount + 1u != polyline.Stations.size() ||
        !std::isfinite(worldX) || !std::isfinite(worldZ))
        return ScanClosestStation(polyline, worldX, worldZ);

    const int64 cx = CellAlong(worldX, grid.OriginX, grid.CellSize, grid.CellsX);
    const int64 cz = CellAlong(worldZ, grid.OriginZ, grid.CellSize, grid.CellsZ);
    const int64 lastX = static_cast<int64>(grid.CellsX) - 1;
    const int64 lastZ = static_cast<int64>(grid.CellsZ) - 1;
    const float32 slack = grid.Slack + kSlackRelative * (std::abs(worldX) + std::abs(worldZ));

    uint32 best = std::numeric_limits<uint32>::max();
    SegmentHit bestHit{std::numeric_limits<float32>::infinity(), 0.0f};

    // Rings of cells around the query's cell, outward. After ring r, every
    // unvisited cell lies outside the square of side 2r + 1 cells, so its segments
    // are at least the query's distance to that square's nearest open side away.
    for (int64 r = 0;; ++r)
    {
        const int64 x0 = cx - r, x1 = cx + r, z0 = cz - r, z1 = cz + r;
        for (int64 z = std::max<int64>(z0, 0); z <= std::min(z1, lastZ); ++z)
        {
            const bool edgeRow = z == z0 || z == z1;
            for (int64 x = std::max<int64>(x0, 0); x <= std::min(x1, lastX); ++x)
                if (edgeRow || x == x0 || x == x1)
                    TestCell(polyline, grid, static_cast<std::size_t>(z) * grid.CellsX + static_cast<std::size_t>(x),
                             worldX, worldZ, best, bestHit);
        }

        float32 unvisited = std::numeric_limits<float32>::infinity();
        if (x0 > 0)
            unvisited = std::min(unvisited, worldX - (grid.OriginX + static_cast<float32>(x0) * grid.CellSize));
        if (x1 < lastX)
            unvisited = std::min(unvisited, grid.OriginX + static_cast<float32>(x1 + 1) * grid.CellSize - worldX);
        if (z0 > 0)
            unvisited = std::min(unvisited, worldZ - (grid.OriginZ + static_cast<float32>(z0) * grid.CellSize));
        if (z1 < lastZ)
            unvisited = std::min(unvisited, grid.OriginZ + static_cast<float32>(z1 + 1) * grid.CellSize - worldZ);
        if (unvisited == std::numeric_limits<float32>::infinity() || bestHit.Distance < unvisited - slack)
            break;
    }

    if (best == std::numeric_limits<uint32>::max())
    {
        RouteSample none{};
        none.Distance = std::numeric_limits<float32>::infinity();
        return none;
    }
    return SampleOnSegment(polyline, best, bestHit);
}

void RoutePolylineBoundsXZ(const RoutePolyline& polyline, float32 falloff,
                           float32& outMinX, float32& outMinZ,
                           float32& outMaxX, float32& outMaxZ)
{
    outMinX = std::numeric_limits<float32>::max();
    outMinZ = std::numeric_limits<float32>::max();
    outMaxX = -std::numeric_limits<float32>::max();
    outMaxZ = -std::numeric_limits<float32>::max();

    if (!polyline.IsValid())
        return;

    for (const RouteStation& station : polyline.Stations)
    {
        // Each station's own half-width, not the route's largest: the box is a
        // union of per-station discs, which is tight on a route that tapers.
        const float32 reach = station.HalfWidth + falloff;
        outMinX = std::min(outMinX, station.X - reach);
        outMinZ = std::min(outMinZ, station.Z - reach);
        outMaxX = std::max(outMaxX, station.X + reach);
        outMaxZ = std::max(outMaxZ, station.Z + reach);
    }
}

} // namespace GameEngine::TerrainECS
