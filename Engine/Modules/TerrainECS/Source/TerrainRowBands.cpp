#include "TerrainRowBands.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::TerrainECS
{

int32 AxisSample(float32 value, float32 origin, float32 size, uint32 samples, bool roundUp)
{
    const float32 sample = Math::Clamp01((value - origin) / size) * (samples - 1);
    return static_cast<int32>(roundUp ? std::ceil(sample) : std::floor(sample));
}

uint32 RunRowBands(JobSystem::WorkStealingThreadPool* pool, std::span<const BakeBandRegion> regions,
                   const std::function<void(std::size_t, int32, int32, int32, int32)>& rowFn)
{
    uint64 totalTexels = 0;
    for (const BakeBandRegion& r : regions)
        totalTexels += static_cast<uint64>(r.MaxX - r.MinX + 1) * static_cast<uint64>(r.MaxZ - r.MinZ + 1);
    if (pool == nullptr || totalTexels < kParallelBakeMinTexels)
    {
        for (std::size_t i = 0; i < regions.size(); ++i)
            rowFn(i, regions[i].MinX, regions[i].MaxX, regions[i].MinZ, regions[i].MaxZ);
        return 0;
    }
    // Flatten every region into fixed-height row bands so the pool stays busy even when there are
    // few regions. One ParallelFor unit per band: the wave scheduler runs a multi-system wave's
    // members on pool workers, and a bake is one of them, so the fork must be legal on a worker,
    // which ParallelFor is.
    struct Band { std::size_t Region; int32 MinX, MaxX, Z0, Z1; };
    std::vector<Band> bands;
    for (std::size_t i = 0; i < regions.size(); ++i)
        for (int32 z = regions[i].MinZ; z <= regions[i].MaxZ; z += kBakeBandRows)
            bands.push_back(Band{i, regions[i].MinX, regions[i].MaxX, z,
                                 std::min(z + kBakeBandRows - 1, regions[i].MaxZ)});
    JobSystem::ParallelFor(pool, bands.size(), [&rowFn, &bands](std::size_t i) {
        const Band& band = bands[i];
        rowFn(band.Region, band.MinX, band.MaxX, band.Z0, band.Z1);
    });
    return static_cast<uint32>(bands.size());
}

} // namespace GameEngine::TerrainECS
