#include "PagePyramidBuilder.h"

#include "HeightPageCodec.h"
#include "PageCodec.h"
#include "PageFieldFilter.h"
#include "PageStreaming/PageStoreWriter.h"
#include "PageStoreSerialization.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Mathematics/Matrix3x3.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <limits>

namespace GameEngine::PageStreaming
{
namespace
{

// Pages of a row encoded at once before they are written: enough work for the pool, a bounded
// buffer (64 pages of at most 67.6 KB).
constexpr uint32 kPagesEncodedPerChunk = 64u;

using FinerRows = std::array<const uint8*, 3>;

const float32* Heights(const uint8* row)
{
    return reinterpret_cast<const float32*>(row);
}

// The tent across a row at coarser sample `i`: [1 2 1] / 4 over the finer samples 2i - 1, 2i, 2i + 1.
float32 TentAcross(const float32* row, uint32 i, uint32 width)
{
    const int64 center = 2 * static_cast<int64>(i);
    return 0.25f * row[ClampIndex(center - 1, width)] + 0.5f * row[ClampIndex(center, width)] +
           0.25f * row[ClampIndex(center + 1, width)];
}

void HeightTentRow(const FinerRows& rows, uint32 finerWidth, float32* out, uint32 width)
{
    for (uint32 i = 0; i < width; ++i)
        out[i] = 0.25f * TentAcross(Heights(rows[0]), i, finerWidth) +
                 0.5f * TentAcross(Heights(rows[1]), i, finerWidth) +
                 0.25f * TentAcross(Heights(rows[2]), i, finerWidth);
}

FilterTaps GatherTaps(const FinerRows& rows, uint32 i, uint32 finerWidth, uint32 texelBytes)
{
    const int64 center = 2 * static_cast<int64>(i);
    FilterTaps taps{};
    for (uint32 r = 0; r < 3u; ++r)
        for (uint32 c = 0; c < 3u; ++c)
            taps[r * 3u + c] = rows[r] + static_cast<std::size_t>(ClampIndex(center - 1 + c, finerWidth)) * texelBytes;
    return taps;
}

// The three values in ascending order, summed from the smallest.
float32 AscendingSum(Mathematics::Vector3 values)
{
    std::sort(std::begin(values.v), std::end(values.v));
    return (values.v[0] + values.v[1]) + values.v[2];
}

} // namespace

float32 CubeFaceTent(const Mathematics::Matrix3x3& taps)
{
    const float32 neighbors = (taps[1][0] + taps[1][2]) + (taps[0][1] + taps[2][1]);
    const float32 diagonals = (taps[0][0] + taps[2][2]) + (taps[2][0] + taps[0][2]);
    return (0.25f * taps[1][1] + 0.125f * neighbors) + 0.0625f * diagonals;
}

float32 CubeCornerTent(float32 corner, const Mathematics::Vector3& edges, const Mathematics::Vector3& diagonals)
{
    constexpr float32 kCornerWeight = 5.0f / 16.0f;
    constexpr float32 kEdgeWeight = 1.0f / 6.0f;
    constexpr float32 kDiagonalWeight = 1.0f / 16.0f;
    return (kCornerWeight * corner + kEdgeWeight * AscendingSum(edges)) + kDiagonalWeight * AscendingSum(diagonals);
}

PagePyramidBuilder::PagePyramidBuilder(PageStoreWriter& writer, bool patching, JobSystem::WorkStealingThreadPool* pool,
                                       const PageFaceBuild* face)
    : m_Writer(writer)
    , m_Patching(patching)
    , m_Pool(pool)
    , m_FaceBuild(face)
    , m_Face(face ? face->Face : uint8{0})
{
    const PageStoreHeader& header = writer.Layout().Header;
    m_Codec = MakePageCodec(header);
    m_Format = header.Format;
    m_Filter = header.Filter;
    m_TexelBytes = PageTexelBytes(m_Format);
    m_Stride = PageStrideTexels(m_Format);
    m_Apron = (m_Stride - kPageOwnedSamples) / 2u;
    // A format and filter that no kind uses have no samples to make: the build fails at Finish.
    if (!m_Codec || !PageEncodingKind(m_Format, m_Filter))
        m_Failed.store(true);
    for (const PageStoreLevel& shape : writer.Layout().Levels)
    {
        LevelState state;
        state.Shape = shape;
        if (IsHeight())
        {
            const std::size_t pages = static_cast<std::size_t>(shape.PagesX) * shape.PagesZ;
            state.PageMin.assign(pages, std::numeric_limits<float32>::max());
            state.PageMax.assign(pages, std::numeric_limits<float32>::lowest());
        }
        m_Levels.push_back(std::move(state));
    }
    if (m_FaceBuild && m_FaceBuild->Capture)
    {
        m_FaceBuild->Capture->Levels.resize(m_Levels.size());
        for (std::size_t level = 0; level < m_Levels.size(); ++level)
            for (std::vector<float32>& line : m_FaceBuild->Capture->Levels[level])
                line.assign(m_Levels[level].Shape.SamplesX, 0.0f);
    }
}

PagePyramidBuilder::~PagePyramidBuilder() = default;

void PagePyramidBuilder::PushLevel0Rows(const uint8* rows, uint32 rowCount)
{
    if (m_Failed.load())
        return;
    LevelState& level0 = m_Levels.front();
    const std::size_t rowBytes = static_cast<std::size_t>(level0.Shape.SamplesX) * m_TexelBytes;
    assert(level0.RowsReceived + rowCount <= level0.Shape.SamplesZ);
    const std::size_t firstNew = level0.Rows.size();
    for (uint32 r = 0; r < rowCount; ++r)
        level0.Rows.emplace_back(rows + r * rowBytes, rows + (r + 1u) * rowBytes);
    if (IsHeight())
        RunParallel(rowCount, [&](uint32 r) {
            Quantize(0, reinterpret_cast<float32*>(level0.Rows[firstNew + r].data()), level0.Shape.SamplesX);
        });
    level0.RowsReceived += rowCount;
    CaptureInnerLines(0, level0.RowsReceived - rowCount, rowCount);
    Advance(0);
}

bool PagePyramidBuilder::Finish() const
{
    if (m_Failed.load())
        return false;
    for (const LevelState& level : m_Levels)
        if (level.RowsReceived != level.Shape.SamplesZ || level.NextPageRow != level.Shape.PagesZ)
            return false;
    return true;
}

bool PagePyramidBuilder::IsHeight() const
{
    return m_Format == PageFieldFormat::HeightSteps;
}

const uint8* PagePyramidBuilder::Row(const LevelState& level, int64 row) const
{
    const uint32 clamped = ClampIndex(row, level.Shape.SamplesZ);
    assert(clamped >= level.FirstKeptRow && clamped < level.RowsReceived);
    return level.Rows[clamped - level.FirstKeptRow].data();
}

void PagePyramidBuilder::Advance(uint32 levelIndex)
{
    LevelState& level = m_Levels[levelIndex];
    const uint32 lastRow = level.Shape.SamplesZ - 1u;
    // A page row is complete once its last row (the far apron, or the field's last row) is in.
    while (level.NextPageRow < level.Shape.PagesZ)
    {
        const uint32 needed =
            std::min(level.NextPageRow * kPageOwnedSamples + kPageOwnedSamples - 1u + m_Apron, lastRow);
        if (level.RowsReceived <= needed)
            break;
        EmitPageRow(levelIndex, level.NextPageRow++);
    }
    if (levelIndex + 1u < m_Levels.size())
    {
        // A coarser row j reads rows 2j - 1 to 2j + 1 of this level, clamped to the field.
        const uint32 coarserRows = m_Levels[levelIndex + 1u].Shape.SamplesZ;
        uint32 end = level.NextChildRow;
        while (end < coarserRows && std::min(2u * end + 1u, lastRow) < level.RowsReceived)
            ++end;
        if (end > level.NextChildRow)
        {
            const uint32 first = level.NextChildRow;
            MakeCoarserRows(levelIndex, first, end - first);
            level.NextChildRow = end;
            Advance(levelIndex + 1u);
        }
    }
    Discard(levelIndex);
}

void PagePyramidBuilder::EmitPageRow(uint32 levelIndex, uint32 pageRow)
{
    // A chunk of the row is encoded in parallel, then written in X order: the writer places pages
    // in the order they arrive, so the store's bytes do not depend on the workers.
    if (m_FaceBuild && m_FaceBuild->Capture)
        return;
    const uint32 pagesX = m_Levels[levelIndex].Shape.PagesX;
    std::vector<EncodedPage> encoded(std::min(pagesX, kPagesEncodedPerChunk));
    std::vector<uint8> changed(encoded.size());
    for (uint32 first = 0; first < pagesX; first += kPagesEncodedPerChunk)
    {
        const uint32 count = std::min(kPagesEncodedPerChunk, pagesX - first);
        RunParallel(count, [&](uint32 i) {
            changed[i] = EncodePage(levelIndex, first + i, pageRow, encoded[i]) ? 1u : 0u;
        });
        for (uint32 i = 0; i < count; ++i)
        {
            if (changed[i] == 0)
            {
                m_PagesUnchanged.fetch_add(1);
                continue;
            }
            if (!m_Writer.WritePage(PageAddress{m_Face, static_cast<uint8>(levelIndex), first + i, pageRow}, encoded[i]))
            {
                m_Failed.store(true);
                return;
            }
            m_PagesWritten.fetch_add(1);
        }
    }
}

bool PagePyramidBuilder::EncodePage(uint32 levelIndex, uint32 pageX, uint32 pageZ, EncodedPage& encoded)
{
    const LevelState& level = m_Levels[levelIndex];
    const uint32 width = level.Shape.SamplesX;
    std::vector<uint8> samples(static_cast<std::size_t>(m_Stride) * m_Stride * m_TexelBytes);
    const int64 firstX = static_cast<int64>(pageX) * kPageOwnedSamples - m_Apron;
    const int64 firstZ = static_cast<int64>(pageZ) * kPageOwnedSamples - m_Apron;
    if (m_FaceBuild)
    {
        // A cube store holds float32 heights.
        assert(m_TexelBytes == sizeof(float32));
        for (uint32 row = 0; row < m_Stride; ++row)
        {
            auto* out = reinterpret_cast<float32*>(samples.data()) + static_cast<std::size_t>(row) * m_Stride;
            for (uint32 column = 0; column < m_Stride; ++column)
                out[column] = FaceSample(levelIndex, firstX + column, firstZ + row);
        }
    }
    else
    {
        for (uint32 row = 0; row < m_Stride; ++row)
        {
            const uint8* source = Row(level, firstZ + row);
            uint8* out = samples.data() + static_cast<std::size_t>(row) * m_Stride * m_TexelBytes;
            for (uint32 column = 0; column < m_Stride; ++column)
                std::memcpy(out + static_cast<std::size_t>(column) * m_TexelBytes,
                            source + static_cast<std::size_t>(ClampIndex(firstX + column, width)) * m_TexelBytes, m_TexelBytes);
        }
    }

    PageFootprint footprint;
    if (IsHeight())
        footprint = BoundHeightPage(levelIndex, pageX, pageZ, samples);
    m_Codec->Encode(levelIndex, samples, footprint, encoded);
    const PageAddress address{m_Face, static_cast<uint8>(levelIndex), pageX, pageZ};
    const PageIndexEntry* current = m_Writer.Layout().FindPresent(address);
    return !(m_Patching && current && current->Hash == encoded.Hash && current->MinWord == encoded.MinWord &&
             current->MaxWord == encoded.MaxWord && current->Flags == encoded.Flags);
}

PageFootprint PagePyramidBuilder::BoundHeightPage(uint32 levelIndex, uint32 pageX, uint32 pageZ,
                                                  std::span<const uint8> samples)
{
    LevelState& level = m_Levels[levelIndex];
    const float32* heights = Heights(samples.data());
    // The footprint: the owned samples and the shared edge after them (stride indices 1 to 129).
    float32 lowest = std::numeric_limits<float32>::max();
    float32 highest = std::numeric_limits<float32>::lowest();
    for (uint32 r = kPageApronSamples; r < kPageStrideSamples; ++r)
    {
        for (uint32 c = kPageApronSamples; c < kPageStrideSamples; ++c)
        {
            const float32 value = heights[static_cast<std::size_t>(r) * kPageStrideSamples + c];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }
    if (levelIndex > 0)
    {
        // The level-0 truth under the footprint: the finer pages it covers (clamped to the finer
        // level, whose last page also holds the samples past the field's edge).
        const LevelState& finer = m_Levels[levelIndex - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
        {
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const uint32 childX = std::min(2u * pageX + dx, finer.Shape.PagesX - 1u);
                const uint32 childZ = std::min(2u * pageZ + dz, finer.Shape.PagesZ - 1u);
                const std::size_t child = static_cast<std::size_t>(childZ) * finer.Shape.PagesX + childX;
                lowest = std::min(lowest, finer.PageMin[child]);
                highest = std::max(highest, finer.PageMax[child]);
            }
        }
    }
    const std::size_t pageIndex = static_cast<std::size_t>(pageZ) * level.Shape.PagesX + pageX;
    level.PageMin[pageIndex] = lowest;
    level.PageMax[pageIndex] = highest;
    return {lowest, highest};
}

void PagePyramidBuilder::MakeCoarserRows(uint32 levelIndex, uint32 firstRow, uint32 rowCount)
{
    LevelState& coarser = m_Levels[levelIndex + 1u];
    std::vector<std::vector<uint8>> rows(rowCount);
    RunParallel(rowCount, [&](uint32 r) { MakeCoarserRow(levelIndex, firstRow + r, rows[r]); });
    for (std::vector<uint8>& row : rows)
        coarser.Rows.push_back(std::move(row));
    coarser.RowsReceived += rowCount;
    CaptureInnerLines(levelIndex + 1u, coarser.RowsReceived - rowCount, rowCount);
}

void PagePyramidBuilder::MakeCoarserRow(uint32 levelIndex, uint32 row, std::vector<uint8>& out) const
{
    const LevelState& finer = m_Levels[levelIndex];
    const uint32 finerWidth = finer.Shape.SamplesX;
    const uint32 width = m_Levels[levelIndex + 1u].Shape.SamplesX;
    const int64 center = 2 * static_cast<int64>(row);
    const FinerRows rows{Row(finer, center - 1), Row(finer, center), Row(finer, center + 1)};
    out.resize(static_cast<std::size_t>(width) * m_TexelBytes);
    if (m_Filter == PageFieldFilter::HeightTent)
    {
        float32* heights = reinterpret_cast<float32*>(out.data());
        if (m_FaceBuild)
            MakeFaceRow(levelIndex, row, heights, width);
        else
            HeightTentRow(rows, finerWidth, heights, width);
        Quantize(levelIndex + 1u, heights, width);
        return;
    }
    for (uint32 i = 0; i < width; ++i)
        FilterCoarserTexel(m_Filter, GatherTaps(rows, i, finerWidth, m_TexelBytes), m_TexelBytes,
                           out.data() + static_cast<std::size_t>(i) * m_TexelBytes);
}

float32 PagePyramidBuilder::FaceSample(uint32 levelIndex, int64 x, int64 z) const
{
    const LevelState& level = m_Levels[levelIndex];
    const int64 last = static_cast<int64>(level.Shape.SamplesX) - 1;
    x = std::clamp<int64>(x, -1, last + 1);
    z = std::clamp<int64>(z, -1, last + 1);
    const bool pastX = x < 0 || x > last;
    const bool pastZ = z < 0 || z > last;
    // Past a corner no face has a sample: the corner sample repeats. Without ghosts (the capture
    // pass) a side's own sample stands in: only the sides' samples read it, and the capture keeps
    // none of them.
    if ((pastX && pastZ) || ((pastX || pastZ) && !m_FaceBuild->Ghosts))
        return Heights(Row(level, std::clamp<int64>(z, 0, last)))[std::clamp<int64>(x, 0, last)];
    if (!pastX && !pastZ)
        return Heights(Row(level, z))[x];
    const auto& lines = m_FaceBuild->Ghosts->Levels[levelIndex];
    if (pastZ)
        return lines[static_cast<std::size_t>(z < 0 ? PageFaceSide::MinZ : PageFaceSide::MaxZ)][static_cast<std::size_t>(x)];
    return lines[static_cast<std::size_t>(x < 0 ? PageFaceSide::MinX : PageFaceSide::MaxX)][static_cast<std::size_t>(z)];
}

void PagePyramidBuilder::MakeFaceRow(uint32 levelIndex, uint32 row, float32* out, uint32 width) const
{
    const uint32 last = width - 1u;
    const int64 centerZ = 2 * static_cast<int64>(row);
    for (uint32 column = 0; column < width; ++column)
    {
        const int64 centerX = 2 * static_cast<int64>(column);
        if ((column == 0 || column == last) && (row == 0 || row == last))
        {
            // Outward from the corner along X and Z. The sample one past the corner along X and the
            // one past it along Z are both the third edge's sample next to the corner.
            const int64 outwardX = column == 0 ? -1 : 1;
            const int64 outwardZ = row == 0 ? -1 : 1;
            out[column] = CubeCornerTent(
                FaceSample(levelIndex, centerX, centerZ),
                {FaceSample(levelIndex, centerX - outwardX, centerZ), FaceSample(levelIndex, centerX, centerZ - outwardZ),
                 FaceSample(levelIndex, centerX + outwardX, centerZ)},
                {FaceSample(levelIndex, centerX - outwardX, centerZ - outwardZ),
                 FaceSample(levelIndex, centerX + outwardX, centerZ - outwardZ),
                 FaceSample(levelIndex, centerX - outwardX, centerZ + outwardZ)});
            continue;
        }
        Mathematics::Matrix3x3 taps;
        for (int32 dz = -1; dz <= 1; ++dz)
            for (int32 dx = -1; dx <= 1; ++dx)
                taps[dx + 1][dz + 1] = FaceSample(levelIndex, centerX + dx, centerZ + dz);
        out[column] = CubeFaceTent(taps);
    }
}

void PagePyramidBuilder::CaptureInnerLines(uint32 levelIndex, uint32 firstRow, uint32 rowCount)
{
    if (!m_FaceBuild || !m_FaceBuild->Capture)
        return;
    const LevelState& level = m_Levels[levelIndex];
    const uint32 samples = level.Shape.SamplesX;
    auto& lines = m_FaceBuild->Capture->Levels[levelIndex];
    for (uint32 z = firstRow; z < firstRow + rowCount; ++z)
    {
        const float32* row = Heights(Row(level, z));
        if (z == 1u)
            std::copy(row, row + samples, lines[static_cast<std::size_t>(PageFaceSide::MinZ)].begin());
        if (z == samples - 2u)
            std::copy(row, row + samples, lines[static_cast<std::size_t>(PageFaceSide::MaxZ)].begin());
        lines[static_cast<std::size_t>(PageFaceSide::MinX)][z] = row[1];
        lines[static_cast<std::size_t>(PageFaceSide::MaxX)][z] = row[samples - 2u];
    }
}

void PagePyramidBuilder::Quantize(uint32 levelIndex, float32* values, std::size_t count) const
{
    const float32 step = m_Writer.Layout().Header.StepPerLevel[levelIndex];
    for (std::size_t i = 0; i < count; ++i)
        values[i] = QuantizeHeight(values[i], step);
}

void PagePyramidBuilder::Discard(uint32 levelIndex)
{
    LevelState& level = m_Levels[levelIndex];
    const int64 rows = level.Shape.SamplesZ;
    int64 keepFrom = level.NextPageRow < level.Shape.PagesZ
                         ? static_cast<int64>(level.NextPageRow) * kPageOwnedSamples - m_Apron
                         : rows;
    if (levelIndex + 1u < m_Levels.size() && level.NextChildRow < m_Levels[levelIndex + 1u].Shape.SamplesZ)
        keepFrom = std::min<int64>(keepFrom, 2 * static_cast<int64>(level.NextChildRow) - 1);
    // A clamped read of a row past the field's end reads the last row, so it stays until the end.
    keepFrom = std::clamp<int64>(keepFrom, 0, std::min<int64>(level.RowsReceived, rows - 1));
    if (level.NextPageRow == level.Shape.PagesZ &&
        (levelIndex + 1u == m_Levels.size() || level.NextChildRow == m_Levels[levelIndex + 1u].Shape.SamplesZ))
        keepFrom = level.RowsReceived;
    while (level.FirstKeptRow < keepFrom && !level.Rows.empty())
    {
        level.Rows.pop_front();
        ++level.FirstKeptRow;
    }
}

void PagePyramidBuilder::RunParallel(uint32 count, const std::function<void(uint32)>& body) const
{
    JobSystem::ParallelFor(m_Pool, count, [&body](size_t i) { body(static_cast<uint32>(i)); });
}

} // namespace GameEngine::PageStreaming
