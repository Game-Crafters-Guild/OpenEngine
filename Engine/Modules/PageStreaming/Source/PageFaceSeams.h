#pragma once

#include "PagePyramidBuilder.h"

#include <array>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// The seams of an EqualAngleCube store at every level: each face's sides and the lines one in from
/// them, from which each face's ghosts are read. Each level follows from the level below's seams
/// only, and two faces compute their shared side bit for bit alike.
class PageFaceSeams
{
public:
    /// `levels` and `steps` are the store's.
    PageFaceSeams(std::span<const PageStoreLevel> levels, const std::array<float32, kPageStoreMaxLevels>& steps);

    /// A face's inner lines from its capture pass and its four sides at level 0, quantized to the
    /// level's step, indexed by PageFaceSide.
    void SetFace(uint8 face, PageFaceLines inner, std::array<std::vector<float32>, kPageFaceSides> level0Sides);

    /// Computes every coarser level's sides and the inner lines' end samples, which lie on the
    /// sides across them. Call once every face is set.
    void Build();

    /// The ghosts of `face`: past each side, the neighbor's inner line in this face's order.
    PageFaceLines Ghosts(uint8 face) const;

private:
    using Lines = std::array<std::vector<float32>, kPageFaceSides>;

    /// The sample at (x, z) of a face's level, x and z in [-1, samples]; only a side, a line one in
    /// from a side, or one past a side.
    float32 Sample(uint8 face, uint32 level, int64 x, int64 z) const;
    float32 GhostSample(uint8 face, uint32 level, PageFaceSide side, int64 along) const;
    float32 CoarserSideSample(uint8 face, uint32 level, int64 x, int64 z) const;
    void FillInnerEnds(uint8 face, uint32 level);

    std::vector<PageStoreLevel> m_Levels;
    std::array<float32, kPageStoreMaxLevels> m_Steps{};
    std::array<std::vector<Lines>, kPageStoreMaxFaces> m_Sides; // [face][level]
    std::array<PageFaceLines, kPageStoreMaxFaces> m_Inner;
};

} // namespace GameEngine::PageStreaming
