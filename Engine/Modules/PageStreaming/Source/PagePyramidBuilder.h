#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::Mathematics
{
struct Matrix3x3;
struct Vector3;
} // namespace GameEngine::Mathematics

namespace GameEngine::PageStreaming
{

class PageCodec;
class PageStoreWriter;
struct EncodedPage;
struct PageFootprint;

/// A cube face's samples one in from each of its sides, at every level: Levels[level][side] holds
/// the level's samples of row 1, row N - 2, column 1 or column N - 2 (by PageFaceSide), indexed
/// along the side.
struct PageFaceLines
{
    std::vector<std::array<std::vector<float32>, kPageFaceSides>> Levels;
};

/// What a builder of one face of an EqualAngleCube store reads past the face and records of it.
struct PageFaceBuild
{
    uint8 Face = 0;
    /// The samples one past each side at every level, in this face's order along the side: the
    /// neighbor face's samples one in from the shared edge (PageFaceNeighbor). Null while the
    /// face's own inner lines are captured.
    const PageFaceLines* Ghosts = nullptr;
    /// When set, the builder records the face's inner lines here and writes no page. A sample one
    /// in from a side never reads past the face, so the lines need no ghosts.
    PageFaceLines* Capture = nullptr;
};

/// The tent on a cube face: 4, 2 and 1 over 16 on the center, edge and diagonal taps, summed in
/// opposite pairs, so two faces that see one footprint rotated or mirrored give the same bits.
/// taps[1 + dx][1 + dz] is the sample at offset (dx, dz): a 3 x 3 grid that the callers fill by offset.
float32 CubeFaceTent(const Mathematics::Matrix3x3& taps);

/// The tent at a cube corner, where three faces meet: 5/16 on the corner sample, 1/6 on each of
/// the three edge samples next to it and 1/16 on each face's diagonal sample, each set summed in
/// ascending order, so all three faces give one value, bit for bit.
float32 CubeCornerTent(float32 corner, const Mathematics::Vector3& edges, const Mathematics::Vector3& diagonals);

/// Builds a store's pyramid from its level-0 rows, pushed in order, with memory bounded by a few
/// page rows per level, for any of the store's formats: the store's codec encodes each page and its
/// filter makes each coarser level. Each level keeps only the rows its next page row and the next
/// level's next rows still read: a page row is written as soon as its rows and apron are in, and a
/// coarser row is made with the filter as soon as its three source rows are. A coarser level is the
/// filter of the samples of the level below, so a recook of unchanged input reproduces every page.
/// On a Bc5 store those samples are the rows before their BC5 encode, not what the level below
/// stores: a coarser level is refiltered from the baker's rows, never from decoded pages.
/// A height store quantizes every value to its level's step before anything reads it, so its
/// coarser level is the filter of what the level below stores, and bounds each page by the level-0
/// values under its footprint.
class PagePyramidBuilder
{
public:
    /// Writes into `writer`. When `patching`, a page whose bytes hash to the writer's entry is not
    /// written. `face` is null on a flat store (the apron clamps at the edge); on a cube store it names
    /// the face, whose apron and filter read the ghosts past its sides.
    PagePyramidBuilder(PageStoreWriter& writer, bool patching, JobSystem::WorkStealingThreadPool* pool,
                       const PageFaceBuild* face);
    ~PagePyramidBuilder();

    /// Appends the next `rowCount` level-0 rows: SamplesX samples of PageTexelBytes(format) bytes
    /// each (a height store's samples are float32 values in the source's units).
    void PushLevel0Rows(const uint8* rows, uint32 rowCount);

    /// True when every row was pushed and every page written.
    bool Finish() const;

    uint64 PagesWritten() const { return m_PagesWritten.load(); }
    uint64 PagesUnchanged() const { return m_PagesUnchanged.load(); }

private:
    struct LevelState
    {
        PageStoreLevel Shape;
        uint32 RowsReceived = 0;
        uint32 FirstKeptRow = 0;
        std::deque<std::vector<uint8>> Rows; // rows [FirstKeptRow, RowsReceived)
        uint32 NextPageRow = 0;
        uint32 NextChildRow = 0; // rows of the next coarser level made so far
        std::vector<float32> PageMin; // a height store's, per page: level-0 truth and the page's own footprint
        std::vector<float32> PageMax;
    };

    bool IsHeight() const;
    const uint8* Row(const LevelState& level, int64 row) const;
    void Advance(uint32 level);
    void EmitPageRow(uint32 level, uint32 pageRow);
    /// Encodes a page; false when a patch already holds exactly these bytes.
    bool EncodePage(uint32 level, uint32 pageX, uint32 pageZ, EncodedPage& encoded);
    /// The bounds of a height page and the level-0 values under its footprint, kept for its parent.
    PageFootprint BoundHeightPage(uint32 level, uint32 pageX, uint32 pageZ, std::span<const uint8> samples);
    void MakeCoarserRows(uint32 level, uint32 firstRow, uint32 rowCount);
    void MakeCoarserRow(uint32 level, uint32 row, std::vector<uint8>& out) const;
    /// A cube face's sample at (x, z) of a level, x and z in [-1, samples]: the ghosts past a side,
    /// the corner sample past two.
    float32 FaceSample(uint32 level, int64 x, int64 z) const;
    void MakeFaceRow(uint32 level, uint32 row, float32* out, uint32 width) const;
    void CaptureInnerLines(uint32 level, uint32 firstRow, uint32 rowCount);
    void Quantize(uint32 level, float32* values, std::size_t count) const;
    void Discard(uint32 level);
    void RunParallel(uint32 count, const std::function<void(uint32)>& body) const;

    PageStoreWriter& m_Writer;
    bool m_Patching = false;
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    const PageFaceBuild* m_FaceBuild = nullptr;
    uint8 m_Face = 0;
    std::unique_ptr<PageCodec> m_Codec;
    PageFieldFormat m_Format = PageFieldFormat::HeightSteps;
    PageFieldFilter m_Filter = PageFieldFilter::HeightTent;
    uint32 m_TexelBytes = 0;
    uint32 m_Stride = 0; // samples a page stores per axis
    uint32 m_Apron = 0;  // apron samples on each side
    std::vector<LevelState> m_Levels;
    std::atomic<uint64> m_PagesWritten{0};
    std::atomic<uint64> m_PagesUnchanged{0};
    std::atomic<bool> m_Failed{false};
};

} // namespace GameEngine::PageStreaming
