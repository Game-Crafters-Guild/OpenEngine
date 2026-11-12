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

class PageResidencyManager;

/// True when page `address` reads a level-0 sample of `level0Rect`, with the samples of its level up to
/// `marginSamples` past its owned area (kPageApronSamples: a height page).
bool PageReadsLevel0Rect(const PageAddress& address, const PageSampleRect& level0Rect, uint32 marginSamples);

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

/// A level-0 rect, or none, on each face of a layer.
using PageFaceRects = std::array<std::optional<PageSampleRect>, kPageStoreMaxFaces>;

/// Adds `rect` to face `face`'s rect of `rects`.
void IncludePageFaceRect(PageFaceRects& rects, uint8 face, const PageSampleRect& rect);

/// One page of a layer as it was or is, for undo. The layer holds no such page when Deltas is empty.
struct HeightPageLayerPage
{
    PageAddress Address;
    bool Written = false; ///< a page of the writes made at its level (AddAtLevel), not of the composed pyramid
    std::vector<float32> Deltas;
};

/// The work of a layer's apron refreshes: the pages whose apron it refreshed, and the page searches
/// those refreshes made to read the samples their aprons store.
struct HeightPageLayerApronWork
{
    uint64 Pages = 0;
    uint64 Lookups = 0;
};

/// The height deltas a terrain adds to its base pages, held per page, on one face or a cube's six
/// (design page-streaming-planets 3.6, 3.13). A page of level k adds the writes made at level k, the
/// [1 2 1] / 4 tent of the finer levels (the store's filter, so a base page plus the layer's page is
/// the page the cooker cuts from the edited level 0) and the bilinear prolongation of the writes made
/// at coarser levels. All three are linear, so writes add and undo is a page snapshot.
class HeightPageLayer
{
public:
    /// An empty layer of `faceCount` faces (1 or kPageStoreMaxFaces) of the pyramid `levels`.
    HeightPageLayer(uint32 faceCount, std::span<const PageStoreLevel> levels);

    /// Replaces level-0 rect `rect` of face `face` with `deltas` (row-major over the rect) and refilters
    /// the coarser levels where the rect reaches them. Main thread: the height pages read the layer
    /// after its owner writes it in the same frame.
    void WriteLevel0(uint8 face, const PageSampleRect& rect, std::span<const float32> deltas);

    /// The finest level whose grid holds a footprint `level0Samples` level-0 samples across in at most
    /// kWritePagesAcross pages a side; level 0 for a smaller footprint, the coarsest level for a larger one.
    uint32 WriteLevel(uint32 level0Samples) const;
    /// Adds `deltas` (row-major over rect `rect` of level `level` of face `face`) to the layer and
    /// refilters the coarser levels where the rect reaches them. Returns the level-0 rect whose samples
    /// the write changes. Main thread, as WriteLevel0.
    PageSampleRect AddAtLevel(uint8 face, uint32 level, const PageSampleRect& rect, std::span<const float32> deltas);
    /// Replaces the writes made at level `level` (AddAtLevel, ReplaceAtLevel) over rect `rect` of face `face`
    /// with `values` (row-major over the rect) and refilters the coarser levels where the rect reaches
    /// them. The writes at the other levels stay. Returns the level-0 rect whose samples the write
    /// changes. Main thread, as WriteLevel0.
    PageSampleRect ReplaceAtLevel(uint8 face, uint32 level, const PageSampleRect& rect, std::span<const float32> values);

    /// Adds the layer's page `address` to `samples` (kPageSampleCount, row-major, apron included);
    /// false, and no change, when the layer adds nothing there.
    bool ApplyToPage(const PageAddress& address, std::span<float32> samples) const;
    /// Adds the layer to the height window of page `address`, laid out as FillPlanetWindow lays it out;
    /// past a face's side the neighbor face's delta, past a cube corner or a flat edge the edge delta.
    /// False, and no change, when the layer adds nothing there or the span is not that size.
    bool ApplyToWindow(const PageAddress& address, uint32 margin, std::span<float32> samples) const;
    /// The deltas of page `address` (kPageSampleCount, row-major, apron included) from the writes at its
    /// level and the finer levels, or empty. A layer with coarse writes (AddAtLevel above level 0) adds
    /// more: read it through ApplyToPage.
    std::span<const float32> PageDeltas(const PageAddress& address) const;
    /// The layer's delta at level-0 point (x, z) of face `face`, in samples: the bilinear blend of the
    /// four level-0 samples around it, as ApplyToPage composes them. Main thread, as the writes.
    float32 SampleLevel0(uint8 face, float64 x, float64 z) const;

    /// Starts to keep every page a write changes, as it was before the first change.
    void BeginCapture();
    /// The pages changed since BeginCapture, as they were before; ends the capture.
    std::vector<HeightPageLayerPage> TakeCapture();
    /// The pages `keys` name, as they are now.
    std::vector<HeightPageLayerPage> CopyPages(std::span<const HeightPageLayerPage> keys) const;
    /// Puts `pages` back byte for byte and appends to `changed` the face rects whose samples that changes.
    void RestorePages(std::span<const HeightPageLayerPage> pages, std::vector<PageFaceRect>& changed);

    /// The face rects whose resident pages a write of `rect` on face `face` changes, for pages that read
    /// `marginSamples` past their owned area: the rect, and on a planet each neighbor face's edge band
    /// whose pages read a sample the write reaches.
    void RefreshRects(uint8 face, const PageSampleRect& rect, uint32 marginSamples,
                      std::vector<PageFaceRect>& out) const;
    /// The level-0 face rects a write of `rect` on face `face` holds: the rect, and on a planet the
    /// neighbor face's edge samples along each side within one sample of the rect. The coarser levels'
    /// bands are RefreshRects' only: they never widen a write.
    void WriteRects(uint8 face, const PageSampleRect& rect, std::vector<PageFaceRect>& out) const;
    /// The level-0 bounds of every write on face `face` since the layer was made.
    const std::optional<PageSampleRect>& Extent(uint8 face) const { return m_Extents[face]; }

    /// True when the layer was made for the pyramid `levels`: the same levels and level-0 samples.
    bool FitsLevels(std::span<const PageStoreLevel> levels) const;
    uint32 FaceCount() const { return m_FaceCount; }
    /// The pyramid the layer was made for.
    std::span<const PageStoreLevel> Levels() const { return m_Levels; }
    /// The bytes of the pages the layer holds. A page whose deltas are all zero is not held.
    uint64 Bytes() const;
    /// The apron refresh work of every write since the layer was made.
    const HeightPageLayerApronWork& ApronWork() const { return m_ApronWork; }

    /// The pages a side a footprint may span at its write level (design page-streaming-planets 3.13 item 6).
    static constexpr uint32 kWritePagesAcross = 4;

private:
    friend class HeightLayerAuthoredPages;
    friend struct HeightPageLayerTestAccess;
    using PageMap = FlatMap<PageAddress, std::vector<float32>>;

    // The pages a save keeps: each level-0 page, and each page of the writes made at a coarser level
    // (Written). Every other page is the tent of these.
    std::vector<HeightPageLayerPage> ExportAuthoredPages() const;
    // Replaces the layer's pages with `pages` (ExportAuthoredPages of a layer of the same faces and
    // pyramid) and composes the coarser levels, so each page equals the exported layer's bit for bit.
    // False, and an empty layer, when a page is outside the layer, is a composed page above level 0,
    // or is not kPageSampleCount samples. Not during a capture.
    bool ImportAuthoredPages(std::span<const HeightPageLayerPage> pages);

    PageFaceSample StoredSample(uint8 face, uint32 level, int64 x, int64 z) const;
    // The sample a height window holds at (x, z) of `face`, up to `margin` past a side (PageCubeWindowSampleAt).
    PageFaceSample WindowSample(uint8 face, uint32 level, int64 x, int64 z, uint32 margin) const;
    // Adds to `window` the layer's samples past side `side` of the window of `address`, which reads
    // every sample past that side (not past a cube corner) from one rect of the neighbor face.
    void AddPastSide(const PageAddress& address, uint32 margin, PageFaceSide side, std::span<float32> window) const;
    // The rect of the neighbor face that the window of `address` reads at its samples `strip`, which all
    // lie past one side (PastSideStrip). Sets `neighbor` to that face.
    PageSampleRect PastSideRect(const PageAddress& address, uint32 margin, const PageSampleRect& strip,
                                uint8& neighbor) const;
    // True when a page or a prolonged write reaches a sample the window of `address` reads: in its face,
    // and on a cube past each side. False means ApplyToWindow adds zero there.
    bool WindowReached(const PageAddress& address, uint32 margin) const;
    // True when a page of `m_Pages` that Gather reads for rect `rect`, or a prolonged write, reaches the rect.
    bool RectReached(uint8 face, uint32 level, const PageSampleRect& rect) const;
    // Sets `out` to rect `rect` of level `level` of face `face` of `pages`, zero where no page is held.
    // Returns the pages it searched.
    static uint32 Gather(const PageMap& pages, uint8 face, uint32 level, const PageSampleRect& rect,
                         std::vector<float32>& out);
    // Gather into `out`: `out` holds the rect's samples, row-major, and is zero on entry.
    static uint32 GatherInto(const PageMap& pages, uint8 face, uint32 level, const PageSampleRect& rect,
                             std::span<float32> out);
    void Scatter(bool written, uint8 face, uint32 level, const PageSampleRect& rect, std::span<const float32> values);
    void AddToRect(bool written, uint8 face, uint32 level, const PageSampleRect& rect, std::span<const float32> deltas);
    // Adds the writes of every level coarser than `level`, prolonged to `level`, over `rect` (in the face).
    void AddProlongedWrites(uint8 face, uint32 level, const PageSampleRect& rect, std::span<float32> out) const;
    bool ProlongedWritesReach(uint8 face, uint32 level, const PageSampleRect& rect) const;
    void AddProlongedPage(const PageAddress& address, std::span<float32> samples) const;
    void KeepForCapture(bool written, const PageAddress& address);
    PageSampleRect FilterLevel(uint8 face, uint32 level, const PageSampleRect& finerReach);
    // Sets rect `rect` of level `level` (1 or coarser) to the tent of the finer level plus the writes
    // at `level`: every write keeps this true bit for bit, so ImportAuthoredPages composes the same bits.
    void ComposeLevel(uint8 face, uint32 level, const PageSampleRect& rect);
    void Clear();
    void RefreshAprons(uint8 face, uint32 level, const PageSampleRect& reach);
    // A page's apron holds the samples past its own: past a cube face side, the neighbor face's
    // sample one in (PageCubeSampleAt); past a flat terrain's edge, the edge sample.
    void RefreshPageApron(const PageAddress& address);
    // Sets `out` to the samples a page stores past side `side` of face `face` at level `level`, at samples
    // alongMin to alongMax along that side: on a cube, the neighbor face's samples one in from the side
    // (PageCubeSampleAt); on a flat terrain, the edge samples.
    void ReadPastSide(uint8 face, uint32 level, PageFaceSide side, uint32 alongMin, uint32 alongMax,
                      std::vector<float32>& out);
    // Sets stride sample `index` of page `address` (held at `page`, or null) to `value`. A page the layer
    // does not hold is made for a non-zero value only.
    void StoreApronSample(const PageAddress& address, std::vector<float32>*& page, std::size_t index, float32 value);
    void DropZeroPages();

    std::vector<PageStoreLevel> m_Levels;
    uint32 m_FaceCount = 1;
    PageMap m_Pages;   // the composed pyramid: each level's writes plus the tent of the finer levels
    PageMap m_Written; // the writes made at levels 1 and coarser, owned samples only
    std::vector<uint32> m_WrittenPages; // the m_Written pages of each level
    PageFaceRects m_Extents;
    std::vector<PageAddress> m_Touched;        // m_Pages pages a write changed, checked for all-zero at its end
    std::vector<PageAddress> m_TouchedWritten; // the same for m_Written
    bool m_Capturing = false;
    PageMap m_CapturedPages;   // pre-images; an empty vector: no page
    PageMap m_CapturedWritten;
    std::vector<float32> m_Finer;       // reused by ComposeLevel and AddToRect
    std::vector<float32> m_Coarser;     // reused by ComposeLevel
    std::vector<float32> m_Line;        // reused by RefreshPageApron
    HeightPageLayerApronWork m_ApronWork;
};

/// Rewrites the pages of `stream` (layers `held` at `heldVersion`, pages reading `marginSamples` past their
/// owned area) for the layers `requested`: when `changedSince` is `heldVersion` the pages that read
/// `changed`, else every page a held or requested layer reaches. `scratch` is reused.
void RefreshLayeredPages(PageResidencyManager& residency, uint32 stream, std::span<const HeightPageLayer* const> held,
                         uint64 heldVersion, std::span<const HeightPageLayer* const> requested,
                         const PageFaceRects& changed, uint64 changedSince, uint32 marginSamples,
                         std::vector<PageFaceRect>& scratch);

} // namespace GameEngine::PageStreaming
