#include "SpatialGrid/Grid.h"

#include "SpatialGrid/GridOverlap.h"
#include "Memory/ScratchScope.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace GameEngine::SpatialGrid
{

namespace
{

// A bound widens by this share of its magnitude, so that a rounded subtract never moves it past a cell edge.
constexpr float64 kBoundMargin = 1.0 / 281474976710656.0; // 2^-48
// A cell range of this many cells or more on one axis repeats a tag, so the query scans the level.
constexpr float64 kTagPeriod = 1024.0;
// A query reads its cells in a level that has this many movers or more for each cell. With fewer movers, a
// test of every mover of the level costs less than the cells do.
constexpr float64 kCellWalkMoversPerCell = 4.0;
constexpr uint64 kHashAxisMask = (uint64{1} << 21) - 1;
constexpr uint32 kTagAxisMask = 1023u;
// The benchmark target builds a copy of the grid with cell keys on X and Z only (design note, section 7, arm D).
#ifndef GE_SPATIAL_GRID_PLANAR_KEYS
#define GE_SPATIAL_GRID_PLANAR_KEYS 0
#endif
constexpr bool kPlanarKeys = GE_SPATIAL_GRID_PLANAR_KEYS != 0;
// The hit scratch of a query starts at this many hits and doubles when it is full.
constexpr size_t kFirstScratchHits = 64;

#if GE_DEBUG_INSTRUMENTATION
thread_local size_t t_LastQueryHitCapacity = 0;
#endif

// Doubles the hit scratch, to at least 64 hits. It grows in place while `hits` is the latest take of `scratch`.
std::span<GridHit> GrowHits(Memory::ScratchScope& scratch, std::span<GridHit> hits)
{
    return scratch.GrowArray(hits, std::max(hits.size() * 2, kFirstScratchHits));
}

struct CellCoord
{
    int64 X;
    int64 Y;
    int64 Z;
};

struct Bounds
{
    Mathematics::WorldPosition Min;
    Mathematics::WorldPosition Max;
};

CellCoord CellOf(const Mathematics::WorldPosition& p, float64 inverseCellSize)
{
    return {static_cast<int64>(std::floor(p.X * inverseCellSize)),
            kPlanarKeys ? int64{0} : static_cast<int64>(std::floor(p.Y * inverseCellSize)),
            static_cast<int64>(std::floor(p.Z * inverseCellSize))};
}

uint64 HashOf(int64 x, int64 y, int64 z)
{
    const uint64 packed = (static_cast<uint64>(x) & kHashAxisMask) |
                          ((static_cast<uint64>(y) & kHashAxisMask) << 21) |
                          ((static_cast<uint64>(z) & kHashAxisMask) << 42);
    return Hashing::SplitMix64(packed);
}

uint32 TagOf(int64 x, int64 y, int64 z)
{
    return (static_cast<uint32>(x) & kTagAxisMask) | ((static_cast<uint32>(y) & kTagAxisMask) << 10) |
           ((static_cast<uint32>(z) & kTagAxisMask) << 20);
}

float64 LowCell(float64 value, float64 widen, float64 inverseCellSize)
{
    const float64 low = value - widen;
    return std::floor((low - std::abs(low) * kBoundMargin) * inverseCellSize);
}

float64 HighCell(float64 value, float64 widen, float64 inverseCellSize)
{
    const float64 high = value + widen;
    return std::floor((high + std::abs(high) * kBoundMargin) * inverseCellSize);
}

Bounds AroundPoint(const Mathematics::WorldPosition& p, float64 x, float64 y, float64 z)
{
    return {{p.X - x, p.Y - y, p.Z - z}, {p.X + x, p.Y + y, p.Z + z}};
}

struct SphereShape
{
    Mathematics::WorldPosition Center;
    float64 Radius;

    Bounds GetBounds() const { return AroundPoint(Center, Radius, Radius, Radius); }
    bool Test(const Mathematics::WorldPosition& p, float64 r, float64& key) const
    {
        key = DistanceSquared(Center, p);
        return OverlapsSphere(Center, Radius, p, r);
    }
};

struct CylinderShape
{
    Mathematics::WorldPosition Center;
    float64 Radius;
    float64 HalfHeight;

    Bounds GetBounds() const { return AroundPoint(Center, Radius, HalfHeight, Radius); }
    bool Test(const Mathematics::WorldPosition& p, float64 r, float64& key) const
    {
        key = DistanceSquared(Center, p);
        return OverlapsCylinder(Center, Radius, HalfHeight, p, r);
    }
};

struct BoxShape
{
    Mathematics::WorldPosition Center;
    Mathematics::Vector3 HalfExtents;

    Bounds GetBounds() const { return AroundPoint(Center, HalfExtents.x, HalfExtents.y, HalfExtents.z); }
    bool Test(const Mathematics::WorldPosition& p, float64 r, float64& key) const
    {
        key = DistanceSquared(Center, p);
        return OverlapsBox(Center, HalfExtents, p, r);
    }
};

struct ConeShape
{
    Mathematics::WorldPosition Apex;
    Mathematics::Vector3 Direction;
    float64 Range;
    float64 CosHalfAngle;
    float64 SinHalfAngle;

    Bounds GetBounds() const { return AroundPoint(Apex, Range, Range, Range); }
    bool Test(const Mathematics::WorldPosition& p, float64 r, float64& key) const
    {
        key = DistanceSquared(Apex, p);
        return OverlapsCone(Apex, Direction, Range, CosHalfAngle, SinHalfAngle, p, r);
    }
};

struct SegmentShape
{
    Mathematics::WorldPosition Start;
    Mathematics::WorldPosition End;
    float64 Radius;

    Bounds GetBounds() const
    {
        return {{std::min(Start.X, End.X) - Radius, std::min(Start.Y, End.Y) - Radius,
                 std::min(Start.Z, End.Z) - Radius},
                {std::max(Start.X, End.X) + Radius, std::max(Start.Y, End.Y) + Radius,
                 std::max(Start.Z, End.Z) + Radius}};
    }
    bool Test(const Mathematics::WorldPosition& p, float64 r, float64& key) const
    {
        return OverlapsSegment(Start, End, Radius, p, r, key);
    }
};

// An axis of a position and of a cell coordinate.
struct Axis
{
    float64 Mathematics::WorldPosition::* Position;
    int64 CellCoord::* Cell;
};

constexpr Axis kAxes[3] = {{&Mathematics::WorldPosition::X, &CellCoord::X},
                           {&Mathematics::WorldPosition::Y, &CellCoord::Y},
                           {&Mathematics::WorldPosition::Z, &CellCoord::Z}};
// The segment's parameter range of a column grows by this much, so that a rounded divide never drops a cell.
constexpr float64 kColumnMargin = 1e-12;

// The cell range on `axis` of the part of the segment between parameters t0 and t1, widened by `widen`.
void SegmentPartCells(const SegmentShape& shape, const Axis& axis, float64 t0, float64 t1, float64 widen,
                      float64 inverseCellSize, float64& low, float64& span)
{
    const float64 start = shape.Start.*axis.Position;
    const float64 delta = shape.End.*axis.Position - start;
    const float64 a = start + t0 * delta;
    const float64 b = start + t1 * delta;
    low = LowCell(std::min(a, b), widen, inverseCellSize);
    span = HighCell(std::max(a, b), widen, inverseCellSize) - low + 1.0;
}

// Calls visitColumn(major, lowB, spanB, lowC, spanC) once for each cell column on the segment's major axis
// that the segment widened by `widen` can touch. Each column holds the cells of that part of the segment.
template <typename VisitColumn>
void ForEachSegmentColumn(const SegmentShape& shape, const Axis& major, const Axis& minorB, const Axis& minorC,
                          float64 widen, float64 cellSize, float64 inverseCellSize, VisitColumn&& visitColumn)
{
    const float64 start = shape.Start.*major.Position;
    const float64 delta = shape.End.*major.Position - start;
    const float64 low = LowCell(std::min(start, start + delta), widen, inverseCellSize);
    const float64 high = HighCell(std::max(start, start + delta), widen, inverseCellSize);
    for (float64 column = low; column <= high; column += 1.0)
    {
        float64 t0 = 0.0;
        float64 t1 = 1.0;
        if (delta != 0.0)
        {
            const float64 enter = (column * cellSize - widen - start) / delta;
            const float64 leave = ((column + 1.0) * cellSize + widen - start) / delta;
            t0 = std::max(0.0, std::min(enter, leave) - kColumnMargin);
            t1 = std::min(1.0, std::max(enter, leave) + kColumnMargin);
            if (t0 > t1)
                continue;
        }
        float64 lowB = 0.0;
        float64 spanB = 0.0;
        float64 lowC = 0.0;
        float64 spanC = 0.0;
        SegmentPartCells(shape, minorB, t0, t1, widen, inverseCellSize, lowB, spanB);
        SegmentPartCells(shape, minorC, t0, t1, widen, inverseCellSize, lowC, spanC);
        visitColumn(column, lowB, spanB, lowC, spanC);
    }
}

// Adds the cells of each segment column to `Cells`, and clears `TagsFit` when a column spans a tag period.
struct SegmentColumnCount
{
    bool PlanarB;
    bool PlanarC;
    float64& Cells;
    bool& TagsFit;

    void operator()(float64, float64, float64 spanB, float64, float64 spanC) const
    {
        spanB = PlanarB ? 1.0 : spanB;
        spanC = PlanarC ? 1.0 : spanC;
        Cells += spanB * spanC;
        TagsFit = TagsFit && spanB < kTagPeriod && spanC < kTagPeriod;
    }
};

// Calls OnCell(cell) for each cell of a segment column. A planar axis has the one cell 0.
template <typename OnCell>
struct SegmentColumnCells
{
    const Axis& Major;
    const Axis& MinorB;
    const Axis& MinorC;
    bool PlanarB;
    bool PlanarC;
    OnCell& Visit;

    void operator()(float64 column, float64 lowB, float64 spanB, float64 lowC, float64 spanC) const
    {
        const int64 b0 = PlanarB ? 0 : static_cast<int64>(lowB);
        const int64 c0 = PlanarC ? 0 : static_cast<int64>(lowC);
        const int64 b1 = PlanarB ? 1 : b0 + static_cast<int64>(spanB);
        const int64 c1 = PlanarC ? 1 : c0 + static_cast<int64>(spanC);
        CellCoord cell{};
        cell.*Major.Cell = static_cast<int64>(column);
        for (int64 c = c0; c < c1; ++c)
        {
            cell.*MinorC.Cell = c;
            for (int64 b = b0; b < b1; ++b)
            {
                cell.*MinorB.Cell = b;
                Visit(cell);
            }
        }
    }
};

// The largest error of a unit direction's squared length that a Cone accepts.
constexpr float64 kUnitTolerance = 1e-3;

// True when the query input is in its domain. Debug and DebugFast assert on an input outside it.
bool CheckQueryInput(bool inDomain)
{
    assert(inDomain && "SpatialGrid::Grid: a query input is outside its domain");
    return inDomain;
}

// A NaN radius fails the compare.
bool CheckQueryRadius(float64 radius)
{
    return CheckQueryInput(radius >= 0.0);
}

struct ById
{
    bool operator()(const GridHit& left, const GridHit& right) const { return left.Id < right.Id; }
};

struct ByDistance
{
    bool operator()(const GridHit& left, const GridHit& right) const
    {
        if (left.DistanceSquared != right.DistanceSquared)
            return left.DistanceSquared < right.DistanceSquared;
        return left.Id < right.Id;
    }
};

// Puts the first `kept` hits of `order` at the front, in order. The order is a type, so the sort inlines it.
template <typename Order>
void SortFirst(std::span<GridHit> hits, size_t kept, Order order)
{
    if (kept < hits.size())
        std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(kept), hits.end(), order);
    else
        std::sort(hits.begin(), hits.end(), order);
}

// Merges [a, aEnd) and [b, bEnd), each in ascending id, into `out`; on equal ids `a` comes first. The
// compare selects the source with no branch, because hit ids give the branch predictor no pattern.
void MergeTwo(const GridHit* a, const GridHit* aEnd, const GridHit* b, const GridHit* bEnd, GridHit* out)
{
    while (a != aEnd && b != bEnd)
    {
        const bool takeB = b->Id < a->Id;
        *out++ = *(takeB ? b : a);
        a += takeB ? 0 : 1;
        b += takeB ? 1 : 0;
    }
    out = std::copy(a, aEnd, out);
    std::copy(b, bEnd, out);
}

// Merges the runs of `hits` that end at `ends`, each in ascending id, two by two through `spare`. Returns
// the span that holds all hits in ascending id. The merge writes `ends`.
std::span<GridHit> MergeRuns(std::span<GridHit> hits, std::span<GridHit> spare, std::span<uint32> ends)
{
    size_t runs = ends.size();
    while (runs > 1u)
    {
        size_t begin = 0;
        size_t merged = 0;
        for (size_t run = 0; run < runs; run += 2u)
        {
            const size_t middle = ends[run];
            const size_t end = run + 1u < runs ? ends[run + 1u] : middle;
            MergeTwo(hits.data() + begin, hits.data() + middle, hits.data() + middle, hits.data() + end,
                     spare.data() + begin);
            ends[merged++] = static_cast<uint32>(end);
            begin = end;
        }
        runs = merged;
        std::swap(hits, spare);
    }
    return hits;
}

} // namespace

// The ends of the hit runs of one query, one run per cell. A query merges the runs while each is in
// ascending id and they are at most kMaxRuns; else it sorts its hits.
struct Grid::HitRuns
{
    static constexpr size_t kMaxRuns = 64;

    // Only the first Count entries hold values.
    std::array<uint32, kMaxRuns> Ends;
    uint32 Count = 0u;
    bool Mergeable = false;

    // Records the hits [begin, end) of one run. A level scan is not in ascending id.
    void Close(uint32 begin, uint32 end, bool cellRun)
    {
        if (!Mergeable || end == begin)
            return;
        if (!cellRun || Count == kMaxRuns)
        {
            Mergeable = false;
            return;
        }
        Ends[Count++] = end;
    }
};

Grid::Grid(float64 cellSize)
{
    assert(cellSize >= 1.0 && std::isfinite(cellSize) && std::ldexp(1.0, std::ilogb(cellSize)) == cellSize &&
           "SpatialGrid::Grid: the cell size is a power of two of 1 meter or more");
    float64 size = cellSize;
    for (uint32 level = 0; level < kLevelCount; ++level)
    {
        m_LevelCellSize[level] = size;
        m_LevelInverseCellSize[level] = 1.0 / size;
        size *= 4.0;
    }
}

uint32 Grid::LevelOf(float32 radius) const
{
    for (uint32 level = 0; level + 1 < kLevelCount; ++level)
    {
        if (m_LevelCellSize[level] >= 4.0 * static_cast<float64>(radius))
            return level;
    }
    return kLevelCount - 1;
}

void Grid::Build(std::span<const GridMover> movers)
{
#if GE_DEBUG_INSTRUMENTATION
    m_Building.store(true, std::memory_order_relaxed);
#endif
    const size_t count = movers.size();
    m_Keys.resize(count);
    m_Destinations.resize(count);
    m_LevelMovers.fill(0u);
    m_LevelMaxRadius.fill(0.0f);
    m_CellTags.resize(count);
    bool idOrdered = true;
    // m_Keys holds each mover's cell hash and m_Destinations its level until AssignDestinations.
    for (size_t i = 0; i < count; ++i)
    {
        const GridMover& mover = movers[i];
        assert(mover.Radius >= 0.0f && std::isfinite(mover.Radius) &&
               "SpatialGrid::Grid: a mover radius is NaN, infinite or negative");
        idOrdered = idOrdered && (i == 0u || movers[i - 1u].Id <= mover.Id);
        const uint32 level = LevelOf(mover.Radius);
        const CellCoord cell = CellOf(mover.Position, m_LevelInverseCellSize[level]);
        m_Keys[i] = static_cast<uint32>(HashOf(cell.X, cell.Y, cell.Z));
        m_CellTags[i] = TagOf(cell.X, cell.Y, cell.Z);
        m_Destinations[i] = level;
        ++m_LevelMovers[level];
        m_LevelMaxRadius[level] = std::max(m_LevelMaxRadius[level], mover.Radius);
    }
    m_IdOrdered = idOrdered;
    uint32 slots = 0u;
    for (uint32 level = 0; level < kLevelCount; ++level)
    {
        m_SlotOffset[level] = slots;
        m_SlotCount[level] = m_LevelMovers[level] == 0u ? 0u : std::bit_ceil(2u * m_LevelMovers[level]);
        slots += m_SlotCount[level];
    }
    // AssignDestinations takes one more entry than the run starts keep.
    m_RunStarts.assign(static_cast<size_t>(slots) + 2u, 0u);

    AssignDestinations();

    m_X.resize(count);
    m_Y.resize(count);
    m_Z.resize(count);
    m_Radius.resize(count);
    m_Mask.resize(count);
    m_Id.resize(count);
    m_Tag.resize(count);
    WriteColumns(movers);
#if GE_DEBUG_INSTRUMENTATION
    m_Building.store(false, std::memory_order_relaxed);
#endif
}

void Grid::AssignDestinations()
{
    // A counting sort on the calling thread, in input order: the layout is a function of the input only.
    // The count of bucket K goes to entry K + 2, so that after the prefix sum entry K + 1 holds the start
    // of bucket K, and after the moves entry K holds it.
    const size_t slots = m_RunStarts.size() - 2u;
    for (size_t i = 0; i < m_Keys.size(); ++i)
    {
        const uint32 level = m_Destinations[i];
        m_Keys[i] = m_SlotOffset[level] + (m_Keys[i] & (m_SlotCount[level] - 1u));
        ++m_RunStarts[m_Keys[i] + 2u];
    }
    for (size_t slot = 3; slot < slots + 2u; ++slot)
        m_RunStarts[slot] += m_RunStarts[slot - 1u];
    for (size_t i = 0; i < m_Keys.size(); ++i)
        m_Destinations[i] = m_RunStarts[m_Keys[i] + 1u]++;
    m_RunStarts.pop_back();
}

void Grid::WriteColumns(std::span<const GridMover> movers)
{
    for (size_t i = 0; i < movers.size(); ++i)
    {
        const GridMover& mover = movers[i];
        const uint32 destination = m_Destinations[i];
        m_X[destination] = mover.Position.X;
        m_Y[destination] = mover.Position.Y;
        m_Z[destination] = mover.Position.Z;
        m_Radius[destination] = mover.Radius;
        m_Mask[destination] = mover.Mask;
        m_Id[destination] = mover.Id;
        m_Tag[destination] = m_CellTags[i];
    }
}

void Grid::CheckNotBuilding() const
{
#if GE_DEBUG_INSTRUMENTATION
    assert(!m_Building.load(std::memory_order_relaxed) && "SpatialGrid::Grid: a query ran during Build");
#endif
}

template <typename Shape, typename Visit>
void Grid::VisitCandidates(const Shape& shape, Visit&& visit) const
{
    for (uint32 level = 0; level < kLevelCount; ++level)
    {
        if (m_LevelMovers[level] == 0u)
            continue;
        if constexpr (std::is_same_v<Shape, SegmentShape>)
            VisitSegmentCells(shape, level, visit);
        else
            VisitBoxCells(shape.GetBounds(), level, visit);
    }
}

template <typename Visit>
void Grid::VisitLevel(uint32 level, Visit&& visit) const
{
    const uint32 slotOffset = m_SlotOffset[level];
    visit(m_RunStarts[slotOffset], m_RunStarts[slotOffset + m_SlotCount[level]], false, 0u);
}

template <typename Visit>
void Grid::VisitCell(uint32 level, int64 x, int64 y, int64 z, Visit&& visit) const
{
    const uint32 bucket = m_SlotOffset[level] + static_cast<uint32>(HashOf(x, y, z) & (m_SlotCount[level] - 1u));
    visit(m_RunStarts[bucket], m_RunStarts[bucket + 1u], true, TagOf(x, y, z));
}

template <typename Bounds, typename Visit>
void Grid::VisitBoxCells(const Bounds& bounds, uint32 level, Visit&& visit) const
{
    const float64 widen = m_LevelMaxRadius[level];
    const float64 inverse = m_LevelInverseCellSize[level];
    const float64 lowX = LowCell(bounds.Min.X, widen, inverse);
    const float64 lowY = kPlanarKeys ? 0.0 : LowCell(bounds.Min.Y, widen, inverse);
    const float64 lowZ = LowCell(bounds.Min.Z, widen, inverse);
    const float64 spanX = HighCell(bounds.Max.X, widen, inverse) - lowX + 1.0;
    const float64 spanY = kPlanarKeys ? 1.0 : HighCell(bounds.Max.Y, widen, inverse) - lowY + 1.0;
    const float64 spanZ = HighCell(bounds.Max.Z, widen, inverse) - lowZ + 1.0;
    // A false compare (a NaN or an infinite bound) also takes the scan.
    const bool cellsFit =
        spanX * spanY * spanZ * kCellWalkMoversPerCell <= static_cast<float64>(m_LevelMovers[level]) &&
        spanX < kTagPeriod && spanY < kTagPeriod && spanZ < kTagPeriod;
    if (!cellsFit)
    {
        VisitLevel(level, visit);
        return;
    }
    const int64 x0 = static_cast<int64>(lowX);
    const int64 y0 = static_cast<int64>(lowY);
    const int64 z0 = static_cast<int64>(lowZ);
    for (int64 z = z0; z < z0 + static_cast<int64>(spanZ); ++z)
    {
        for (int64 y = y0; y < y0 + static_cast<int64>(spanY); ++y)
        {
            for (int64 x = x0; x < x0 + static_cast<int64>(spanX); ++x)
                VisitCell(level, x, y, z, visit);
        }
    }
}

template <typename Shape, typename Visit>
void Grid::VisitSegmentCells(const Shape& shape, uint32 level, Visit&& visit) const
{
    const float64 widen = shape.Radius + static_cast<float64>(m_LevelMaxRadius[level]);
    const float64 cellSize = m_LevelCellSize[level];
    const float64 inverse = m_LevelInverseCellSize[level];
    // The major axis is the one along which the segment is longest. Planar keys have one cell in Y.
    const float64 dx = std::abs(shape.End.X - shape.Start.X);
    const float64 dy = kPlanarKeys ? 0.0 : std::abs(shape.End.Y - shape.Start.Y);
    const float64 dz = std::abs(shape.End.Z - shape.Start.Z);
    const uint32 majorIndex = dx >= dy && dx >= dz ? 0u : (dy >= dz ? 1u : 2u);
    const Axis& major = kAxes[majorIndex];
    const Axis& minorB = kAxes[(majorIndex + 1u) % 3u];
    const Axis& minorC = kAxes[(majorIndex + 2u) % 3u];
    const bool planarB = kPlanarKeys && minorB.Cell == &CellCoord::Y;
    const bool planarC = kPlanarKeys && minorC.Cell == &CellCoord::Y;

    const float64 majorLow = LowCell(std::min(shape.Start.*major.Position, shape.End.*major.Position), widen, inverse);
    const float64 majorSpan =
        HighCell(std::max(shape.Start.*major.Position, shape.End.*major.Position), widen, inverse) - majorLow + 1.0;
    float64 cells = 0.0;
    bool tagsFit = majorSpan < kTagPeriod;
    if (tagsFit)
    {
        ForEachSegmentColumn(shape, major, minorB, minorC, widen, cellSize, inverse,
                             SegmentColumnCount{planarB, planarC, cells, tagsFit});
    }
    // A false compare (a NaN or an infinite bound) also takes the scan.
    if (!(tagsFit && cells * kCellWalkMoversPerCell <= static_cast<float64>(m_LevelMovers[level])))
    {
        VisitLevel(level, visit);
        return;
    }
    auto visitCell = [&](const CellCoord& cell) { VisitCell(level, cell.X, cell.Y, cell.Z, visit); };
    ForEachSegmentColumn(shape, major, minorB, minorC, widen, cellSize, inverse,
                         SegmentColumnCells<decltype(visitCell)>{major, minorB, minorC, planarB, planarC, visitCell});
}

bool Grid::Passes(uint32 mover, bool useTag, uint32 tag, const GridFilter& filter) const
{
    return !(useTag && m_Tag[mover] != tag) && (m_Mask[mover] & filter.Mask) != 0u && m_Id[mover] != filter.IgnoreId;
}

template <typename Shape>
void Grid::CollectRun(const Shape& shape, const GridFilter& filter, uint32 begin, uint32 end, bool useTag,
                      uint32 tag, Memory::ScratchScope& scratch, std::span<GridHit>& hits, uint32& matched,
                      HitRuns& runs) const
{
    const uint32 first = matched;
    for (uint32 i = begin; i < end; ++i)
    {
        float64 key = 0.0;
        if (Passes(i, useTag, tag, filter) && shape.Test({m_X[i], m_Y[i], m_Z[i]}, m_Radius[i], key))
        {
            // The visited cells of a level have different tags, and a scan visits a level once.
            assert(matched < m_Id.size() && "SpatialGrid::Grid: a query reported a mover twice");
            if (matched == hits.size())
                hits = GrowHits(scratch, hits);
            hits[matched++] = {m_Id[i], key};
        }
    }
    runs.Close(first, matched, useTag);
}

#if GE_DEBUG_INSTRUMENTATION
size_t Grid::LastQueryHitCapacity()
{
    return t_LastQueryHitCapacity;
}
#endif

template <typename Shape>
void Grid::CountRun(const Shape& shape, const GridFilter& filter, uint32 begin, uint32 end, bool useTag,
                    uint32 tag, uint32& matched) const
{
    for (uint32 i = begin; i < end; ++i)
    {
        if (Passes(i, useTag, tag, filter) && OverlapsSphere(shape.Center, shape.Radius, {m_X[i], m_Y[i], m_Z[i]}, m_Radius[i]))
            ++matched;
    }
}

template <typename Shape>
uint32 Grid::Collect(const Shape& shape, const GridFilter& filter, bool byDistance, std::span<GridHit> out) const
{
    CheckNotBuilding();
#if GE_DEBUG_INSTRUMENTATION
    t_LastQueryHitCapacity = 0;
#endif
    if (m_Id.empty())
        return 0u;

    // The hits grow only in this scope, which the walk keeps innermost.
    Memory::ScratchScope scratch;
    std::span<GridHit> hits;
    uint32 matched = 0u;
    HitRuns runs;
    runs.Mergeable = m_IdOrdered && !byDistance;
    const auto collectRun = [&](uint32 begin, uint32 end, bool useTag, uint32 tag)
    { CollectRun(shape, filter, begin, end, useTag, tag, scratch, hits, matched, runs); };
    VisitCandidates(shape, collectRun);
#if GE_DEBUG_INSTRUMENTATION
    t_LastQueryHitCapacity = hits.size();
#endif
    std::span<GridHit> found = hits.first(matched);
    const size_t kept = std::min(found.size(), out.size());
    if (byDistance)
        SortFirst(found, kept, ByDistance{});
    else if (!runs.Mergeable)
        SortFirst(found, kept, ById{});
    else if (runs.Count > 1u)
        found = MergeRuns(found, scratch.AllocateArrayForOverwrite<GridHit>(matched), {runs.Ends.data(), runs.Count});
    std::copy_n(found.begin(), kept, out.begin());
    return matched;
}

uint32 Grid::Sphere(const Mathematics::WorldPosition& center, float64 radius, const GridFilter& filter,
                    std::span<GridHit> out) const
{
    if (!CheckQueryRadius(radius))
        return 0u;
    return Collect(SphereShape{center, radius}, filter, false, out);
}

uint32 Grid::Cylinder(const Mathematics::WorldPosition& center, float64 radius, float64 halfHeight,
                      const GridFilter& filter, std::span<GridHit> out) const
{
    if (!CheckQueryRadius(radius))
        return 0u;
    return Collect(CylinderShape{center, radius, halfHeight}, filter, false, out);
}

uint32 Grid::Box(const Mathematics::WorldPosition& center, const Mathematics::Vector3& halfExtents,
                 const GridFilter& filter, std::span<GridHit> out) const
{
    return Collect(BoxShape{center, halfExtents}, filter, false, out);
}

uint32 Grid::Cone(const Mathematics::WorldPosition& apex, const Mathematics::Vector3& direction, float64 range,
                  float32 halfAngle, const GridFilter& filter, std::span<GridHit> out) const
{
    const float64 angle = static_cast<float64>(halfAngle);
    const float64 lengthSquared = static_cast<float64>(direction.LengthSquared());
    // The bound is in float32: pi/2 in float32 is above pi/2 in float64.
    if (!CheckQueryRadius(range) ||
        !CheckQueryInput(halfAngle > 0.0f && halfAngle <= std::numbers::pi_v<float32> / 2.0f &&
                         std::abs(lengthSquared - 1.0) <= kUnitTolerance))
        return 0u;
    return Collect(ConeShape{apex, direction, range, std::cos(angle), std::sin(angle)}, filter, false, out);
}

uint32 Grid::Segment(const Mathematics::WorldPosition& start, const Mathematics::WorldPosition& end, float64 radius,
                     const GridFilter& filter, std::span<GridHit> out) const
{
    if (!CheckQueryRadius(radius))
        return 0u;
    return Collect(SegmentShape{start, end, radius}, filter, true, out);
}

uint32 Grid::Nearest(const Mathematics::WorldPosition& center, float64 maxRadius, const GridFilter& filter,
                     std::span<GridHit> out) const
{
    if (!CheckQueryRadius(maxRadius))
        return 0u;
    return Collect(SphereShape{center, maxRadius}, filter, true, out);
}

uint32 Grid::Count(const Mathematics::WorldPosition& center, float64 radius, const GridFilter& filter) const
{
    CheckNotBuilding();
    if (!CheckQueryRadius(radius))
        return 0u;
    const SphereShape shape{center, radius};
    uint32 matched = 0u;
    const auto countRun = [&](uint32 begin, uint32 end, bool useTag, uint32 tag)
    { CountRun(shape, filter, begin, end, useTag, tag, matched); };
    VisitCandidates(shape, countRun);
    return matched;
}

} // namespace GameEngine::SpatialGrid
