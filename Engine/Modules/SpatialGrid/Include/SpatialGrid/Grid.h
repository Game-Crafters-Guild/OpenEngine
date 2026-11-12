#pragma once

#include "SpatialGrid/GridFilter.h"
#include "SpatialGrid/GridHit.h"
#include "SpatialGrid/GridMover.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

#include <array>
#include <span>
#include <vector>

#if GE_DEBUG_INSTRUMENTATION
#include <atomic>
#endif

namespace GameEngine::Memory
{
class ScratchScope;
}

namespace GameEngine::SpatialGrid
{

/// A sparse grid of moving spheres in four levels. Build replaces the set; queries only read, never during Build.
/// A query returns the match count and writes the first hits to `out`: in ascending id, or for Segment and Nearest
/// in ascending distance, then id. Segment walks its cells; a span of 1,024 or more cells on an axis tests the level.
/// A query radius is 0 or more. A query outside its domain asserts in Debug and DebugFast and finds no mover.
class Grid
{
public:
    static constexpr uint32 kLevelCount = 4u;

    /// `cellSize` is the edge of a level 0 cell in meters: a power of two of 1 or more. Use the
    /// diameter of the game's most common query.
    explicit Grid(float64 cellSize = 16.0);

    /// Indexes `movers` in place of the previous set, on the calling thread. Each radius is finite and 0 or more.
    void Build(std::span<const GridMover> movers);

    /// The movers that the sphere overlaps, in ascending id.
    uint32 Sphere(const Mathematics::WorldPosition& center, float64 radius, const GridFilter& filter,
                  std::span<GridHit> out) const;

    /// The movers that the upright cylinder overlaps, in ascending id. The axis is +Y; an infinite
    /// `halfHeight` ignores height.
    uint32 Cylinder(const Mathematics::WorldPosition& center, float64 radius, float64 halfHeight,
                    const GridFilter& filter, std::span<GridHit> out) const;

    /// The movers that the axis-aligned box overlaps, in ascending id.
    uint32 Box(const Mathematics::WorldPosition& center, const Mathematics::Vector3& halfExtents,
               const GridFilter& filter, std::span<GridHit> out) const;

    /// The movers that overlap the solid cone, in ascending id: the points in `range` of the apex and in
    /// `halfAngle` of `direction`. `direction` is a unit vector; `halfAngle` is in radians, in (0, pi/2].
    /// Line of sight stays with the caller.
    uint32 Cone(const Mathematics::WorldPosition& apex, const Mathematics::Vector3& direction, float64 range,
                float32 halfAngle, const GridFilter& filter, std::span<GridHit> out) const;

    /// The movers that a sphere of `radius` swept from `start` to `end` overlaps, in ascending distance
    /// along the segment.
    uint32 Segment(const Mathematics::WorldPosition& start, const Mathematics::WorldPosition& end, float64 radius,
                   const GridFilter& filter, std::span<GridHit> out) const;

    /// The movers that overlap the sphere of `maxRadius`, nearest center first; `out.size()` is the k of a
    /// k-nearest query.
    uint32 Nearest(const Mathematics::WorldPosition& center, float64 maxRadius, const GridFilter& filter,
                   std::span<GridHit> out) const;

    /// The number of movers that the sphere overlaps.
    uint32 Count(const Mathematics::WorldPosition& center, float64 radius, const GridFilter& filter) const;

private:
    friend struct GridTestAccess;
    struct HitRuns;

    template <typename Shape>
    uint32 Collect(const Shape& shape, const GridFilter& filter, bool byDistance, std::span<GridHit> out) const;
    template <typename Shape, typename Visit>
    void VisitCandidates(const Shape& shape, Visit&& visit) const;
    template <typename Visit>
    void VisitLevel(uint32 level, Visit&& visit) const;
    template <typename Visit>
    void VisitCell(uint32 level, int64 x, int64 y, int64 z, Visit&& visit) const;
    template <typename Bounds, typename Visit>
    void VisitBoxCells(const Bounds& bounds, uint32 level, Visit&& visit) const;
    // Walks the cells along the segment, column by column on its major axis, not its bounding box.
    template <typename Shape, typename Visit>
    void VisitSegmentCells(const Shape& shape, uint32 level, Visit&& visit) const;
    // True when the mover at this column index passes the bucket's tag and the filter.
    bool Passes(uint32 mover, bool useTag, uint32 tag, const GridFilter& filter) const;
    // Writes the hits of the movers [begin, end) of one run, and grows `hits` in `scratch` when it is full.
    // `runs` records where the hits of each cell end.
    template <typename Shape>
    void CollectRun(const Shape& shape, const GridFilter& filter, uint32 begin, uint32 end, bool useTag,
                    uint32 tag, Memory::ScratchScope& scratch, std::span<GridHit>& hits, uint32& matched,
                    HitRuns& runs) const;
#if GE_DEBUG_INSTRUMENTATION
    // The hit slots that the last hit query (every query but Count) on the calling thread took.
    static size_t LastQueryHitCapacity();
#endif
    // Counts the movers [begin, end) of one run that overlap the sphere.
    template <typename Shape>
    void CountRun(const Shape& shape, const GridFilter& filter, uint32 begin, uint32 end, bool useTag,
                  uint32 tag, uint32& matched) const;
    uint32 LevelOf(float32 radius) const;
    void AssignDestinations();
    void WriteColumns(std::span<const GridMover> movers);
    void CheckNotBuilding() const;

    std::array<float64, kLevelCount> m_LevelCellSize{};
    std::array<float64, kLevelCount> m_LevelInverseCellSize{};
    // The bucket table of level L is m_RunStarts[m_SlotOffset[L] .. m_SlotOffset[L] + m_SlotCount[L]].
    std::array<uint32, kLevelCount> m_SlotOffset{};
    std::array<uint32, kLevelCount> m_SlotCount{};
    std::array<uint32, kLevelCount> m_LevelMovers{};
    std::array<float32, kLevelCount> m_LevelMaxRadius{};
    // True when the build input is in ascending id: then each bucket's run is in ascending id too.
    bool m_IdOrdered = false;

    // The columns, sorted by level, then by bucket. Capacity grows and never shrinks.
    std::vector<float64> m_X;
    std::vector<float64> m_Y;
    std::vector<float64> m_Z;
    std::vector<float32> m_Radius;
    std::vector<uint32> m_Mask;
    std::vector<uint32> m_Id;
    // The low 10 bits of each cell coordinate: a bucket can hold movers of several cells.
    std::vector<uint32> m_Tag;
    // Run starts per bucket, one more entry than buckets.
    std::vector<uint32> m_RunStarts;
    // Build scratch, in input order: the bucket, the destination and the cell tag of each mover.
    std::vector<uint32> m_Keys;
    std::vector<uint32> m_Destinations;
    std::vector<uint32> m_CellTags;

#if GE_DEBUG_INSTRUMENTATION
    std::atomic<bool> m_Building{false};
#endif
};

} // namespace GameEngine::SpatialGrid
