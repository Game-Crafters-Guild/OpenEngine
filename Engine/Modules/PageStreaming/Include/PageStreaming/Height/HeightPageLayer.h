#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// True when page `address` (its apron included) reads a level-0 sample of `level0Rect`.
bool PageReadsLevel0Rect(const PageAddress& address, const PageSampleRect& level0Rect);

/// The samples of level `level` of `levels` (the store's, BuildPageStoreLevels) whose filtered
/// value reads a sample of `level0Rect`: where a change of those level-0 samples changes that
/// level. `level` is below levels.size().
PageSampleRect Level0RectReach(std::span<const PageStoreLevel> levels, const PageSampleRect& level0Rect,
                               uint32 level);

/// The bytes a one-face HeightPageLayer of `levels` holds at most after writes over `level0Rects`:
/// every page of every level that stores a sample the rects reach, once.
uint64 HeightPageLayerBytesOver(std::span<const PageStoreLevel> levels, std::span<const PageSampleRect> level0Rects);

/// A level-0 rect of one face of a layer.
struct PageFaceRect
{
    uint8 Face = 0;
    PageSampleRect Level0Rect;
};

/// The height deltas a terrain adds to its base pages, held per page, on one face or a cube's six
/// (design page-streaming-planets 3.6). Each coarser level is the store's [1 2 1] / 4 tent of the level
/// below, so a base page plus the layer's page is the page the cooker cuts from the edited level 0.
class HeightPageLayer
{
public:
    /// An empty layer of `faceCount` faces (1 or kPageStoreMaxFaces) of the pyramid `levels`.
    HeightPageLayer(uint32 faceCount, std::span<const PageStoreLevel> levels);

    /// Replaces level-0 rect `rect` of face `face` with `deltas` (row-major over the rect) and refilters
    /// the coarser levels where the rect reaches them. Main thread: the height pages read the layer
    /// after its owner writes it in the same frame.
    void WriteLevel0(uint8 face, const PageSampleRect& rect, std::span<const float32> deltas);

    /// Adds the layer's page `address` to `samples` (kPageSampleCount, row-major, apron included);
    /// false, and no change, when the layer holds no such page.
    bool ApplyToPage(const PageAddress& address, std::span<float32> samples) const;
    /// The deltas of page `address` (kPageSampleCount, row-major, apron included), or empty.
    std::span<const float32> PageDeltas(const PageAddress& address) const;

    /// The face rects whose resident pages a write of `rect` on face `face` changes: the rect, and on
    /// a planet each neighbor face's edge band whose apron reads a sample the write reaches.
    void RefreshRects(uint8 face, const PageSampleRect& rect, std::vector<PageFaceRect>& out) const;
    /// The level-0 bounds of every write on face `face` since the layer was made.
    const std::optional<PageSampleRect>& Extent(uint8 face) const { return m_Extents[face]; }

    /// True when the layer was made for the pyramid `levels`: the same levels and level-0 samples.
    bool FitsLevels(std::span<const PageStoreLevel> levels) const;
    uint32 FaceCount() const { return m_FaceCount; }
    /// The bytes of the pages the layer holds. A page whose deltas are all zero is not held.
    uint64 Bytes() const;

private:
    float32 Sample(uint8 face, uint32 level, uint32 x, uint32 z) const;
    PageFaceSample StoredSample(uint8 face, uint32 level, int64 x, int64 z) const;
    void Gather(uint8 face, uint32 level, const PageSampleRect& rect, std::vector<float32>& out) const;
    void Scatter(uint8 face, uint32 level, const PageSampleRect& rect, std::span<const float32> values);
    PageSampleRect FilterLevel(uint8 face, uint32 level, const PageSampleRect& finerReach);
    void RefreshAprons(uint8 face, uint32 level, const PageSampleRect& reach);
    // A page's apron holds the samples past its own: past a cube face side, the neighbor face's
    // sample one in (PageCubeSampleAt); past a flat terrain's edge, the edge sample.
    void RefreshPageApron(const PageAddress& address);
    void DropZeroPages();

    std::vector<PageStoreLevel> m_Levels;
    uint32 m_FaceCount = 1;
    FlatMap<PageAddress, std::vector<float32>> m_Pages;
    std::array<std::optional<PageSampleRect>, kPageStoreMaxFaces> m_Extents;
    std::vector<PageAddress> m_Touched; // pages a write changed, checked for all-zero at its end
    std::vector<float32> m_Finer;       // reused by FilterLevel
    std::vector<float32> m_Coarser;     // reused by FilterLevel
};

} // namespace GameEngine::PageStreaming
