#include "PageFaceSeams.h"

#include "HeightPageCodec.h"

#include "Mathematics/Matrix3x3.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::PageStreaming
{
namespace
{

std::size_t SideIndex(PageFaceSide side)
{
    return static_cast<std::size_t>(side);
}

} // namespace

PageFaceSeams::PageFaceSeams(std::span<const PageStoreLevel> levels, const std::array<float32, kPageStoreMaxLevels>& steps)
    : m_Levels(levels.begin(), levels.end())
    , m_Steps(steps)
{
    for (std::vector<Lines>& faceSides : m_Sides)
    {
        faceSides.resize(m_Levels.size());
        for (std::size_t level = 0; level < m_Levels.size(); ++level)
            for (std::vector<float32>& line : faceSides[level])
                line.assign(m_Levels[level].SamplesX, 0.0f);
    }
}

void PageFaceSeams::SetFace(uint8 face, PageFaceLines inner, std::array<std::vector<float32>, kPageFaceSides> level0Sides)
{
    m_Inner[face] = std::move(inner);
    m_Sides[face][0] = std::move(level0Sides);
}

void PageFaceSeams::Build()
{
    for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        FillInnerEnds(face, 0);
    for (uint32 level = 1; level < m_Levels.size(); ++level)
    {
        const int64 last = static_cast<int64>(m_Levels[level].SamplesX) - 1;
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
        {
            Lines& sides = m_Sides[face][level];
            for (int64 t = 0; t <= last; ++t)
            {
                sides[SideIndex(PageFaceSide::MinZ)][t] = CoarserSideSample(face, level, t, 0);
                sides[SideIndex(PageFaceSide::MaxZ)][t] = CoarserSideSample(face, level, t, last);
                sides[SideIndex(PageFaceSide::MinX)][t] = CoarserSideSample(face, level, 0, t);
                sides[SideIndex(PageFaceSide::MaxX)][t] = CoarserSideSample(face, level, last, t);
            }
        }
        for (uint8 face = 0; face < kPageStoreMaxFaces; ++face)
            FillInnerEnds(face, level);
    }
}

PageFaceLines PageFaceSeams::Ghosts(uint8 face) const
{
    PageFaceLines ghosts;
    ghosts.Levels.resize(m_Levels.size());
    for (uint32 level = 0; level < m_Levels.size(); ++level)
    {
        const int64 samples = m_Levels[level].SamplesX;
        for (uint32 side = 0; side < kPageFaceSides; ++side)
        {
            std::vector<float32>& line = ghosts.Levels[level][side];
            line.resize(static_cast<std::size_t>(samples));
            for (int64 t = 0; t < samples; ++t)
                line[static_cast<std::size_t>(t)] = GhostSample(face, level, static_cast<PageFaceSide>(side), t);
        }
    }
    return ghosts;
}

float32 PageFaceSeams::Sample(uint8 face, uint32 level, int64 x, int64 z) const
{
    const int64 last = static_cast<int64>(m_Levels[level].SamplesX) - 1;
    const bool pastX = x < 0 || x > last;
    const bool pastZ = z < 0 || z > last;
    if (pastX && pastZ) // past a corner: the corner sample repeats
        return Sample(face, level, std::clamp<int64>(x, 0, last), std::clamp<int64>(z, 0, last));
    if (pastZ)
        return GhostSample(face, level, z < 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ, x);
    if (pastX)
        return GhostSample(face, level, x < 0 ? PageFaceSide::MinX : PageFaceSide::MaxX, z);
    const Lines& sides = m_Sides[face][level];
    if (z == 0 || z == last)
        return sides[SideIndex(z == 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ)][x];
    if (x == 0 || x == last)
        return sides[SideIndex(x == 0 ? PageFaceSide::MinX : PageFaceSide::MaxX)][z];
    const auto& inner = m_Inner[face].Levels[level];
    if (z == 1 || z == last - 1)
        return inner[SideIndex(z == 1 ? PageFaceSide::MinZ : PageFaceSide::MaxZ)][x];
    assert(x == 1 || x == last - 1);
    return inner[SideIndex(x == 1 ? PageFaceSide::MinX : PageFaceSide::MaxX)][z];
}

float32 PageFaceSeams::GhostSample(uint8 face, uint32 level, PageFaceSide side, int64 along) const
{
    const PageFaceEdge edge = PageFaceNeighbor(face, side);
    const int64 last = static_cast<int64>(m_Levels[level].SamplesX) - 1;
    const int64 t = edge.Reversed ? last - along : along;
    return m_Inner[edge.Face].Levels[level][SideIndex(edge.Side)][static_cast<std::size_t>(t)];
}

float32 PageFaceSeams::CoarserSideSample(uint8 face, uint32 level, int64 x, int64 z) const
{
    const uint32 finer = level - 1u;
    const int64 last = static_cast<int64>(m_Levels[level].SamplesX) - 1;
    const int64 cx = 2 * x;
    const int64 cz = 2 * z;
    float32 value = 0.0f;
    if ((x == 0 || x == last) && (z == 0 || z == last))
    {
        // The same footprint as PagePyramidBuilder's corner: outward along X and Z.
        const int64 ox = x == 0 ? -1 : 1;
        const int64 oz = z == 0 ? -1 : 1;
        value = CubeCornerTent(
            Sample(face, finer, cx, cz),
            {Sample(face, finer, cx - ox, cz), Sample(face, finer, cx, cz - oz), Sample(face, finer, cx + ox, cz)},
            {Sample(face, finer, cx - ox, cz - oz), Sample(face, finer, cx + ox, cz - oz), Sample(face, finer, cx - ox, cz + oz)});
    }
    else
    {
        Mathematics::Matrix3x3 taps;
        for (int32 dz = -1; dz <= 1; ++dz)
            for (int32 dx = -1; dx <= 1; ++dx)
                taps[dx + 1][dz + 1] = Sample(face, finer, cx + dx, cz + dz);
        value = CubeFaceTent(taps);
    }
    return QuantizeHeight(value, m_Steps[level]);
}

void PageFaceSeams::FillInnerEnds(uint8 face, uint32 level)
{
    // The ends of a line one in from a side lie on the two sides across it, one in from their ends.
    const std::size_t last = m_Levels[level].SamplesX - 1u;
    const Lines& sides = m_Sides[face][level];
    auto& inner = m_Inner[face].Levels[level];
    for (const auto [line, low, high] : {std::array<PageFaceSide, 3>{PageFaceSide::MinZ, PageFaceSide::MinX, PageFaceSide::MaxX},
                                         std::array<PageFaceSide, 3>{PageFaceSide::MaxZ, PageFaceSide::MinX, PageFaceSide::MaxX},
                                         std::array<PageFaceSide, 3>{PageFaceSide::MinX, PageFaceSide::MinZ, PageFaceSide::MaxZ},
                                         std::array<PageFaceSide, 3>{PageFaceSide::MaxX, PageFaceSide::MinZ, PageFaceSide::MaxZ}})
    {
        const bool nearLow = line == PageFaceSide::MinZ || line == PageFaceSide::MinX;
        const std::size_t offset = nearLow ? 1u : last - 1u;
        inner[SideIndex(line)][0] = sides[SideIndex(low)][offset];
        inner[SideIndex(line)][last] = sides[SideIndex(high)][offset];
    }
}

} // namespace GameEngine::PageStreaming
