#pragma once

#include "Types/Types.h"

#include <cstddef>
#include <functional>
#include <span>

namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine::TerrainECS
{

// One inclusive sample rectangle [MinX, MaxX] x [MinZ, MaxZ] of a bake pass.
struct BakeBandRegion
{
    int32 MinX = 0;
    int32 MinZ = 0;
    int32 MaxX = 0;
    int32 MaxZ = 0;
};

// The sample `value` falls on along an axis of `samples` samples spanning `size` from `origin`,
// rounded down or up: the edge of the sample rectangle that covers a world-space bound. The value
// is clamped to the axis first: a global region's bounds are infinite, and an infinity must never
// be converted to an integer.
int32 AxisSample(float32 value, float32 origin, float32 size, uint32 samples, bool roundUp);

// Row-band height for splitting a large bake across pool workers (RunRowBands). A wide bake is
// dominated by per-texel evaluation that is a pure function of the sample's world position, so
// partitioning a region's rows is byte-identical to one whole-region call (the region-vs-full
// oracles lock it) while keeping every worker busy.
inline constexpr int32 kBakeBandRows = 32;

// Minimum total touched-texel count to justify a parallel dispatch. Below it the serial path runs
// (a brush dab is ~2k texels/tile; the fan-out would cost more than the work); a large-radius
// modifier edit is ~10^5+ texels and fans out.
inline constexpr uint64 kParallelBakeMinTexels = 96u * 1024u;

// Applies rowFn(regionIndex, minX, maxX, z0, z1) over every region. With a pool and at least
// kParallelBakeMinTexels texels in all, the rows are split into kBakeBandRows bands fanned across
// the pool and joined; otherwise each region runs whole on this thread. Each band writes a disjoint
// row range of its region. Legal from a pool worker. Returns the number of bands it fanned out, 0
// when it ran whole.
uint32 RunRowBands(JobSystem::WorkStealingThreadPool* pool, std::span<const BakeBandRegion> regions,
                   const std::function<void(std::size_t, int32, int32, int32, int32)>& rowFn);

} // namespace GameEngine::TerrainECS
