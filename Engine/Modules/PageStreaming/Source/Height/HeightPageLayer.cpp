#include "PageStreaming/Height/HeightPageLayer.h"

#include "Memory/ScratchScope.h"
#include "PageStreaming/PageResidency.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <optional>
#include <utility>

namespace GameEngine::PageStreaming
{

namespace
{

uint32 ClampIndex(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

// The samples of the coarser level whose tent reads a sample of `finer` (the coarser sample j
// reads finer samples 2j - 1 to 2j + 1), within that level's `samplesX` x `samplesZ`.
PageSampleRect CoarserRect(const PageSampleRect& finer, uint32 samplesX, uint32 samplesZ)
{
    PageSampleRect out;
    out.MinX = std::min(finer.MinX / 2u, samplesX - 1u);
    out.MinZ = std::min(finer.MinZ / 2u, samplesZ - 1u);
    out.MaxX = std::min((finer.MaxX + 1u) / 2u, samplesX - 1u);
    out.MaxZ = std::min((finer.MaxZ + 1u) / 2u, samplesZ - 1u);
    return out;
}

// The pages of `pages` along one axis that store a sample of [first, last], their aprons included:
// page p stores samples 128p - 1 to 128p + 128.
void PageSpan(uint32 first, uint32 last, uint32 pages, uint32& minPage, uint32& maxPage)
{
    minPage = first == 0u ? 0u : (first - 1u) / kPageOwnedSamples;
    maxPage = std::min((last + 1u) / kPageOwnedSamples, pages - 1u);
}

std::size_t StrideIndex(uint32 strideX, uint32 strideZ)
{
    return static_cast<std::size_t>(strideZ) * kPageStrideSamples + strideX;
}

// The side of a cube face and the samples along it that a rect within one sample of the side holds.
struct SideReach
{
    bool Near = false;
    PageFaceSide Side = PageFaceSide::MinZ;
    uint32 AlongMin = 0;
    uint32 AlongMax = 0;
};

// The sides of a face of `samples` samples whose neighbor's pages read a sample of `rect`: a page
// past a side reads the samples up to `margin` in from it.
std::array<SideReach, kPageFaceSides> SidesNear(const PageSampleRect& rect, uint32 samples, uint32 margin)
{
    const uint32 inner = samples > margin ? samples - 1u - margin : 0u;
    return {SideReach{rect.MinZ <= margin, PageFaceSide::MinZ, rect.MinX, rect.MaxX},
            SideReach{rect.MaxZ >= inner, PageFaceSide::MaxZ, rect.MinX, rect.MaxX},
            SideReach{rect.MinX <= margin, PageFaceSide::MinX, rect.MinZ, rect.MaxZ},
            SideReach{rect.MaxX >= inner, PageFaceSide::MaxX, rect.MinZ, rect.MaxZ}};
}

// The samples along the neighbor's side of `edge` that hold samples [alongMin, alongMax] of this side.
void NeighborAlong(const PageFaceEdge& edge, uint32 alongMin, uint32 alongMax, uint32 samples, uint32& first,
                   uint32& last)
{
    first = edge.Reversed ? samples - 1u - alongMax : alongMin;
    last = edge.Reversed ? samples - 1u - alongMin : alongMax;
}

bool IsColumnSide(PageFaceSide side)
{
    return side == PageFaceSide::MinX || side == PageFaceSide::MaxX;
}

// The indices of a window of `stride` samples from coordinate `first` whose coordinate is in [low, high];
// empty when the first index is past the last.
std::pair<int64, int64> WindowSpan(int64 first, uint32 stride, int64 low, int64 high)
{
    return {std::max<int64>(low - first, 0), std::min<int64>(high - first, static_cast<int64>(stride) - 1)};
}

// The face sample (x, z) at column 0, row 0 of the window of margin `margin` of page `address`.
std::pair<int64, int64> WindowOrigin(const PageAddress& address, uint32 margin)
{
    return {int64{address.X} * kPageOwnedSamples - margin, int64{address.Z} * kPageOwnedSamples - margin};
}

// The columns (X) and rows (Z) of the window of margin `margin` of page `address` whose samples lie past
// side `side` of the page's square face and not past a corner.
std::optional<PageSampleRect> PastSideStrip(const PageAddress& address, const PageStoreLevel& shape, uint32 margin,
                                            PageFaceSide side)
{
    const uint32 stride = kPageOwnedSamples + 2u * margin;
    const int64 last = static_cast<int64>(shape.SamplesX) - 1;
    const auto [firstX, firstZ] = WindowOrigin(address, margin);
    const bool column = IsColumnSide(side);
    const int64 acrossFirst = column ? firstX : firstZ;
    const int64 alongFirst = column ? firstZ : firstX;
    const bool low = side == PageFaceSide::MinX || side == PageFaceSide::MinZ;
    const auto [across0, across1] = low ? WindowSpan(acrossFirst, stride, acrossFirst, -1)
                                        : WindowSpan(acrossFirst, stride, last + 1, acrossFirst + stride - 1);
    const auto [along0, along1] = WindowSpan(alongFirst, stride, 0, last);
    if (across0 > across1 || along0 > along1)
        return std::nullopt;
    const uint32 acrossMin = static_cast<uint32>(across0);
    const uint32 acrossMax = static_cast<uint32>(across1);
    const uint32 alongMin = static_cast<uint32>(along0);
    const uint32 alongMax = static_cast<uint32>(along1);
    if (column)
        return PageSampleRect{acrossMin, alongMin, acrossMax, alongMax};
    return PageSampleRect{alongMin, acrossMin, alongMax, acrossMax};
}

// The samples of the face of page `address` that its window of margin `margin` reads: the window's
// samples in the face, and each sample past a cube corner or a flat terrain's edge (the edge sample).
PageSampleRect WindowInFace(const PageAddress& address, const PageStoreLevel& shape, uint32 margin)
{
    const uint32 stride = kPageOwnedSamples + 2u * margin;
    const auto [firstX, firstZ] = WindowOrigin(address, margin);
    return PageSampleRect{ClampIndex(firstX, shape.SamplesX), ClampIndex(firstZ, shape.SamplesZ),
                          ClampIndex(firstX + stride - 1, shape.SamplesX),
                          ClampIndex(firstZ + stride - 1, shape.SamplesZ)};
}

// Level `level` sample index `index`, scaled to level 0 and widened by one sample of that level: the
// level-0 samples a change of it reaches by prolongation.
uint32 Level0Low(uint32 index, uint32 level)
{
    const uint64 scaled = static_cast<uint64>(index) << level;
    const uint64 step = uint64{1} << level;
    return static_cast<uint32>(scaled > step ? scaled - step : 0u);
}

uint32 Level0High(uint32 index, uint32 level, uint32 samples0)
{
    const uint64 scaled = (static_cast<uint64>(index) << level) + (uint64{1} << level);
    return static_cast<uint32>(std::min<uint64>(scaled, samples0 - 1u));
}

PageSampleRect Level0Of(const PageSampleRect& rect, uint32 level, uint32 samples0X, uint32 samples0Z)
{
    if (level == 0)
        return rect;
    return PageSampleRect{Level0Low(rect.MinX, level), Level0Low(rect.MinZ, level),
                          Level0High(rect.MaxX, level, samples0X), Level0High(rect.MaxZ, level, samples0Z)};
}

// The samples of the next coarser level a bilinear prolongation to `finer` reads: x reads x / 2 and
// (x + 1) / 2.
PageSampleRect ProlongationSource(const PageSampleRect& finer, uint32 samplesX, uint32 samplesZ)
{
    return PageSampleRect{finer.MinX / 2u, finer.MinZ / 2u, std::min((finer.MaxX + 1u) / 2u, samplesX - 1u),
                          std::min((finer.MaxZ + 1u) / 2u, samplesZ - 1u)};
}

// The samples page `address` stores, apron included, clamped to its level.
PageSampleRect StoredRect(const PageAddress& address, const PageStoreLevel& shape)
{
    const int64 firstX = int64{address.X} * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = int64{address.Z} * kPageOwnedSamples - kPageApronSamples;
    return PageSampleRect{ClampIndex(firstX, shape.SamplesX), ClampIndex(firstZ, shape.SamplesZ),
                          ClampIndex(firstX + kPageStrideSamples - 1, shape.SamplesX),
                          ClampIndex(firstZ + kPageStrideSamples - 1, shape.SamplesZ)};
}

// The samples page `address` owns, clamped to its level.
PageSampleRect OwnedRect(const PageAddress& address, const PageStoreLevel& shape)
{
    const uint32 firstX = address.X * kPageOwnedSamples;
    const uint32 firstZ = address.Z * kPageOwnedSamples;
    return PageSampleRect{firstX, firstZ, std::min(firstX + kPageOwnedSamples - 1u, shape.SamplesX - 1u),
                          std::min(firstZ + kPageOwnedSamples - 1u, shape.SamplesZ - 1u)};
}

// True when a change of samples `reach` of a face can change the apron of page `address` of that face:
// a stored sample the page does not own, or on a flat terrain an edge sample the apron copies past a
// side. Past a cube face side the apron copies the neighbor face, except the corner past two sides.
bool ApronReadsRect(const PageAddress& address, const PageStoreLevel& shape, bool cube, const PageSampleRect& reach)
{
    const PageSampleRect stored = StoredRect(address, shape);
    const PageSampleRect owned = OwnedRect(address, shape);
    const PageSampleRect read{std::max(reach.MinX, stored.MinX), std::max(reach.MinZ, stored.MinZ),
                              std::min(reach.MaxX, stored.MaxX), std::min(reach.MaxZ, stored.MaxZ)};
    if (read.MinX > read.MaxX || read.MinZ > read.MaxZ)
        return false;
    if (read.MinX < owned.MinX || read.MaxX > owned.MaxX || read.MinZ < owned.MinZ || read.MaxZ > owned.MaxZ)
        return true;
    // The page owns every sample of `read`. A page that owns an edge sample lies past that side.
    const bool edgeX = read.MinX == 0u || read.MaxX == shape.SamplesX - 1u;
    const bool edgeZ = read.MinZ == 0u || read.MaxZ == shape.SamplesZ - 1u;
    return cube ? edgeX && edgeZ : edgeX || edgeZ;
}

// A rect of one face at one level.
struct FaceRect
{
    uint8 Face = 0;
    PageSampleRect Rect;
};

void KeepPage(FlatMap<PageAddress, std::vector<float32>>& captured,
              const FlatMap<PageAddress, std::vector<float32>>& pages, const PageAddress& address)
{
    if (captured.Find(address))
        return;
    const std::vector<float32>* page = pages.Find(address);
    captured.InsertOrAssign(address, page ? *page : std::vector<float32>());
}

// Sets `out` to rect `rect` of face `face` and the neighbor edge bands of the first `levelCount` levels.
// Level L's reach on a side changes the neighbor's level-L pages past that side; their level-0
// band is the side's edge sample over the reach scaled to level 0.
void NeighborEdgeRects(std::span<const PageStoreLevel> levels, uint32 levelCount, uint32 margin, uint8 face,
                       const PageSampleRect& rect, std::vector<PageFaceRect>& out)
{
    out.clear();
    out.push_back(PageFaceRect{face, rect});
    if (levelCount == 0)
        return;
    const uint32 samples0 = levels.front().SamplesX;
    std::array<std::optional<PageFaceRect>, kPageFaceSides> bands;
    for (uint32 level = 0; level < levelCount; ++level)
    {
        const uint32 samples = levels[level].SamplesX;
        const std::array<SideReach, kPageFaceSides> sides = SidesNear(Level0RectReach(levels, rect, level), samples, margin);
        for (uint32 sideIndex = 0; sideIndex < kPageFaceSides; ++sideIndex)
        {
            const SideReach& side = sides[sideIndex];
            if (!side.Near)
                continue;
            const PageFaceEdge edge = PageFaceNeighbor(face, side.Side);
            uint32 first = 0, last = 0;
            NeighborAlong(edge, side.AlongMin, side.AlongMax, samples, first, last);
            first = std::min(first << level, samples0 - 1u);
            last = std::min(last << level, samples0 - 1u);
            const uint32 across =
                edge.Side == PageFaceSide::MinX || edge.Side == PageFaceSide::MinZ ? 0u : samples0 - 1u;
            const PageSampleRect band = IsColumnSide(edge.Side) ? PageSampleRect{across, first, across, last}
                                                                : PageSampleRect{first, across, last, across};
            std::optional<PageFaceRect>& held = bands[sideIndex];
            held = held ? PageFaceRect{edge.Face, Union(held->Level0Rect, band)} : PageFaceRect{edge.Face, band};
        }
    }
    for (const std::optional<PageFaceRect>& band : bands)
        if (band)
            out.push_back(*band);
}

} // namespace

void IncludePageFaceRect(PageFaceRects& rects, uint8 face, const PageSampleRect& rect)
{
    std::optional<PageSampleRect>& held = rects[face];
    held = held ? Union(*held, rect) : rect;
}

bool PageReadsLevel0Rect(const PageAddress& address, const PageSampleRect& level0Rect, uint32 marginSamples)
{
    // The page reads its level's samples 128p - m to 128p + 127 + m; level-L sample s sits at level-0
    // sample s * 2^L, and a coarser sample is filtered from the level-0 samples within 2^L - 1 of it.
    const int64 reach = (int64{1} << address.Level) - 1;
    const int64 margin = marginSamples;
    const int64 last = kPageOwnedSamples - 1 + margin;
    const int64 firstX = ((static_cast<int64>(address.X) * kPageOwnedSamples - margin) << address.Level) - reach;
    const int64 firstZ = ((static_cast<int64>(address.Z) * kPageOwnedSamples - margin) << address.Level) - reach;
    const int64 lastX = ((static_cast<int64>(address.X) * kPageOwnedSamples + last) << address.Level) + reach;
    const int64 lastZ = ((static_cast<int64>(address.Z) * kPageOwnedSamples + last) << address.Level) + reach;
    return lastX >= level0Rect.MinX && firstX <= level0Rect.MaxX && lastZ >= level0Rect.MinZ &&
           firstZ <= level0Rect.MaxZ;
}

PageSampleRect Level0RectReach(std::span<const PageStoreLevel> levels, const PageSampleRect& level0Rect,
                               uint32 level)
{
    PageSampleRect rect = level0Rect;
    for (uint32 coarser = 1; coarser <= level; ++coarser)
        rect = CoarserRect(rect, levels[coarser].SamplesX, levels[coarser].SamplesZ);
    return rect;
}

uint64 HeightPageLayerBytesOver(std::span<const PageStoreLevel> levels, std::span<const PageSampleRect> level0Rects)
{
    std::vector<uint64> pages;
    uint64 count = 0;
    for (uint32 level = 0; level < levels.size(); ++level)
    {
        pages.clear();
        for (const PageSampleRect& rect : level0Rects)
        {
            const PageSampleRect reach = Level0RectReach(levels, rect, level);
            uint32 minX = 0, maxX = 0, minZ = 0, maxZ = 0;
            PageSpan(reach.MinX, reach.MaxX, levels[level].PagesX, minX, maxX);
            PageSpan(reach.MinZ, reach.MaxZ, levels[level].PagesZ, minZ, maxZ);
            for (uint32 z = minZ; z <= maxZ; ++z)
                for (uint32 x = minX; x <= maxX; ++x)
                    pages.push_back((static_cast<uint64>(z) << 32u) | x);
        }
        std::sort(pages.begin(), pages.end());
        count += static_cast<uint64>(std::unique(pages.begin(), pages.end()) - pages.begin());
    }
    return count * kPageSampleCount * sizeof(float32);
}

HeightPageLayer::HeightPageLayer(uint32 faceCount, std::span<const PageStoreLevel> levels)
    : m_Levels(levels.begin(), levels.end())
    , m_FaceCount(faceCount)
    , m_WrittenPages(levels.size(), 0u)
{
    assert((faceCount == 1u || faceCount == kPageStoreMaxFaces) && "a layer has one face or a cube's six");
    assert((faceCount == 1u || levels.empty() || levels.front().SamplesX == levels.front().SamplesZ) &&
           "a cube face is square");
}

void HeightPageLayer::WriteLevel0(uint8 face, const PageSampleRect& rect, std::span<const float32> deltas)
{
    assert(face < m_FaceCount && !m_Levels.empty() && rect.MaxX < m_Levels.front().SamplesX &&
           rect.MaxZ < m_Levels.front().SamplesZ && deltas.size() == static_cast<std::size_t>(rect.Width()) * rect.Height());
    if (face >= m_FaceCount || m_Levels.empty())
        return;
    m_Touched.clear();
    m_TouchedWritten.clear();
    Scatter(false, face, 0, rect, deltas);
    RefreshAprons(face, 0, rect);
    PageSampleRect reach = rect;
    for (uint32 level = 1; level < m_Levels.size(); ++level)
    {
        reach = FilterLevel(face, level, reach);
        RefreshAprons(face, level, reach);
    }
    DropZeroPages();
    IncludePageFaceRect(m_Extents, face, rect);
}

uint32 HeightPageLayer::WriteLevel(uint32 level0Samples) const
{
    constexpr uint64 kMostSamples = uint64{kWritePagesAcross} * kPageOwnedSamples;
    uint32 level = 0;
    while (level + 1u < m_Levels.size() &&
           ((uint64{level0Samples} + (uint64{1} << level) - 1u) >> level) > kMostSamples)
        ++level;
    return level;
}

PageSampleRect HeightPageLayer::AddAtLevel(uint8 face, uint32 level, const PageSampleRect& rect,
                                           std::span<const float32> deltas)
{
    assert(face < m_FaceCount && level < m_Levels.size() && rect.MaxX < m_Levels[level].SamplesX &&
           rect.MaxZ < m_Levels[level].SamplesZ &&
           deltas.size() == static_cast<std::size_t>(rect.Width()) * rect.Height());
    if (face >= m_FaceCount || level >= m_Levels.size())
        return rect;
    m_Touched.clear();
    m_TouchedWritten.clear();
    if (level > 0)
    {
        AddToRect(true, face, level, rect, deltas);
        ComposeLevel(face, level, rect);
    }
    else
    {
        AddToRect(false, face, 0, rect, deltas);
    }
    RefreshAprons(face, level, rect);
    PageSampleRect reach = rect;
    for (uint32 coarser = level + 1u; coarser < m_Levels.size(); ++coarser)
    {
        reach = FilterLevel(face, coarser, reach);
        RefreshAprons(face, coarser, reach);
    }
    DropZeroPages();
    const PageSampleRect level0 = Level0Of(rect, level, m_Levels.front().SamplesX, m_Levels.front().SamplesZ);
    IncludePageFaceRect(m_Extents, face, level0);
    return level0;
}

PageSampleRect HeightPageLayer::ReplaceAtLevel(uint8 face, uint32 level, const PageSampleRect& rect,
                                               std::span<const float32> values)
{
    assert(face < m_FaceCount && level < m_Levels.size() &&
           values.size() == static_cast<std::size_t>(rect.Width()) * rect.Height());
    if (face >= m_FaceCount || level >= m_Levels.size())
        return rect;
    // Level 0's composed pages hold its writes alone; a coarser level's writes have their own map.
    std::vector<float32> deltas;
    Gather(level == 0 ? m_Pages : m_Written, face, level, rect, deltas);
    for (std::size_t i = 0; i < deltas.size(); ++i)
        deltas[i] = values[i] - deltas[i];
    return AddAtLevel(face, level, rect, deltas);
}

bool HeightPageLayer::ApplyToPage(const PageAddress& address, std::span<float32> samples) const
{
    if (samples.size() != kPageSampleCount || address.Level >= m_Levels.size())
        return false;
    bool applied = false;
    if (const std::vector<float32>* page = m_Pages.Find(address))
    {
        for (std::size_t i = 0; i < kPageSampleCount; ++i)
            samples[i] += (*page)[i];
        applied = true;
    }
    if (ProlongedWritesReach(address.Face, address.Level, StoredRect(address, m_Levels[address.Level])))
    {
        AddProlongedPage(address, samples);
        applied = true;
    }
    return applied;
}

bool HeightPageLayer::ApplyToWindow(const PageAddress& address, uint32 margin, std::span<float32> samples) const
{
    const uint32 stride = kPageOwnedSamples + 2u * margin;
    if (address.Face >= m_FaceCount || address.Level >= m_Levels.size() ||
        samples.size() != static_cast<std::size_t>(stride) * stride)
        return false;
    if (!WindowReached(address, margin))
        return false;
    const auto [firstX, firstZ] = WindowOrigin(address, margin);
    const PageSampleRect inFace = WindowInFace(address, m_Levels[address.Level], margin);
    Memory::ScratchScope scratch;
    const std::span<float32> term = scratch.AllocateArray<float32>(static_cast<std::size_t>(inFace.Width()) * inFace.Height());
    GatherInto(m_Pages, address.Face, address.Level, inFace, term);
    AddProlongedWrites(address.Face, address.Level, inFace, term);
    const std::span<float32> deltas = scratch.AllocateArray<float32>(samples.size());
    for (uint32 row = 0; row < stride; ++row)
        for (uint32 column = 0; column < stride; ++column)
        {
            const PageFaceSample source = WindowSample(address.Face, address.Level, firstX + column, firstZ + row, margin);
            if (source.Face == address.Face)
                deltas[static_cast<std::size_t>(row) * stride + column] =
                    term[static_cast<std::size_t>(source.Z - inFace.MinZ) * inFace.Width() + (source.X - inFace.MinX)];
        }
    if (m_FaceCount == kPageStoreMaxFaces)
        for (PageFaceSide side : {PageFaceSide::MinZ, PageFaceSide::MaxZ, PageFaceSide::MinX, PageFaceSide::MaxX})
            AddPastSide(address, margin, side, deltas);
    if (std::all_of(deltas.begin(), deltas.end(), [](float32 delta) { return delta == 0.0f; }))
        return false;
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] += deltas[i];
    return true;
}

float32 HeightPageLayer::SampleLevel0(uint8 face, float64 x, float64 z) const
{
    const PageStoreLevel& shape = m_Levels[0];
    if (face >= m_FaceCount || shape.SamplesX < 2u || shape.SamplesZ < 2u)
        return 0.0f;
    const float64 clampedX = std::clamp(x, 0.0, static_cast<float64>(shape.SamplesX - 1u));
    const float64 clampedZ = std::clamp(z, 0.0, static_cast<float64>(shape.SamplesZ - 1u));
    const uint32 lowX = std::min(static_cast<uint32>(clampedX), shape.SamplesX - 2u);
    const uint32 lowZ = std::min(static_cast<uint32>(clampedZ), shape.SamplesZ - 2u);
    const PageSampleRect cell{lowX, lowZ, lowX + 1u, lowZ + 1u};
    std::vector<float32> corners;
    Gather(m_Pages, face, 0, cell, corners);
    AddProlongedWrites(face, 0, cell, corners);
    const float64 fractionX = clampedX - lowX;
    const float64 fractionZ = clampedZ - lowZ;
    const float64 nearRow = corners[0] + fractionX * (corners[1] - corners[0]);
    const float64 farRow = corners[2] + fractionX * (corners[3] - corners[2]);
    return static_cast<float32>(nearRow + fractionZ * (farRow - nearRow));
}

std::span<const float32> HeightPageLayer::PageDeltas(const PageAddress& address) const
{
    const std::vector<float32>* page = m_Pages.Find(address);
    return page ? std::span<const float32>(*page) : std::span<const float32>();
}

void HeightPageLayer::RefreshRects(uint8 face, const PageSampleRect& rect, uint32 marginSamples,
                                   std::vector<PageFaceRect>& out) const
{
    const bool planet = m_FaceCount == kPageStoreMaxFaces && !m_Levels.empty();
    NeighborEdgeRects(m_Levels, planet ? static_cast<uint32>(m_Levels.size()) : 0u, marginSamples, face, rect, out);
}

void HeightPageLayer::WriteRects(uint8 face, const PageSampleRect& rect, std::vector<PageFaceRect>& out) const
{
    const bool planet = m_FaceCount == kPageStoreMaxFaces && !m_Levels.empty();
    NeighborEdgeRects(m_Levels, planet ? 1u : 0u, kPageApronSamples, face, rect, out);
}

void HeightPageLayer::BeginCapture()
{
    m_Capturing = true;
    m_CapturedPages.Clear();
    m_CapturedWritten.Clear();
}

std::vector<HeightPageLayerPage> HeightPageLayer::TakeCapture()
{
    std::vector<HeightPageLayerPage> out;
    out.reserve(m_CapturedPages.Size() + m_CapturedWritten.Size());
    for (const auto& [address, deltas] : m_CapturedPages)
        out.push_back(HeightPageLayerPage{address, false, deltas});
    for (const auto& [address, deltas] : m_CapturedWritten)
        out.push_back(HeightPageLayerPage{address, true, deltas});
    m_Capturing = false;
    m_CapturedPages.Clear();
    m_CapturedWritten.Clear();
    return out;
}

std::vector<HeightPageLayerPage> HeightPageLayer::CopyPages(std::span<const HeightPageLayerPage> keys) const
{
    std::vector<HeightPageLayerPage> out;
    out.reserve(keys.size());
    for (const HeightPageLayerPage& key : keys)
    {
        const std::vector<float32>* page = (key.Written ? m_Written : m_Pages).Find(key.Address);
        out.push_back(HeightPageLayerPage{key.Address, key.Written, page ? *page : std::vector<float32>()});
    }
    return out;
}

void HeightPageLayer::RestorePages(std::span<const HeightPageLayerPage> pages, std::vector<PageFaceRect>& changed)
{
    PageFaceRects rects;
    for (const HeightPageLayerPage& page : pages)
    {
        const PageAddress& address = page.Address;
        if (address.Face >= m_FaceCount || address.Level >= m_Levels.size())
            continue;
        PageMap& map = page.Written ? m_Written : m_Pages;
        const bool held = map.Find(address) != nullptr;
        if (page.Deltas.empty())
        {
            if (held)
                map.Erase(address);
        }
        else
        {
            assert(page.Deltas.size() == kPageSampleCount);
            map.InsertOrAssign(address, page.Deltas);
        }
        if (page.Written && held && page.Deltas.empty())
            --m_WrittenPages[address.Level];
        if (page.Written && !held && !page.Deltas.empty())
            ++m_WrittenPages[address.Level];
        // The level-0 samples the page stores, and those its prolongation reaches.
        IncludePageFaceRect(rects, address.Face,
                            Level0Of(StoredRect(address, m_Levels[address.Level]), address.Level,
                                     m_Levels.front().SamplesX, m_Levels.front().SamplesZ));
    }
    for (uint8 face = 0; face < m_FaceCount; ++face)
    {
        if (!rects[face])
            continue;
        changed.push_back(PageFaceRect{face, *rects[face]});
        IncludePageFaceRect(m_Extents, face, *rects[face]);
    }
}

std::vector<HeightPageLayerPage> HeightPageLayer::ExportAuthoredPages() const
{
    std::vector<HeightPageLayerPage> out;
    for (const auto& [address, deltas] : m_Pages)
        if (address.Level == 0)
            out.push_back(HeightPageLayerPage{address, false, deltas});
    for (const auto& [address, deltas] : m_Written)
        out.push_back(HeightPageLayerPage{address, true, deltas});
    return out;
}

bool HeightPageLayer::ImportAuthoredPages(std::span<const HeightPageLayerPage> pages)
{
    assert(!m_Capturing && "an import replaces every page: no stroke can keep its pre-images");
    Clear();
    for (const HeightPageLayerPage& page : pages)
    {
        const PageAddress& address = page.Address;
        if (address.Face >= m_FaceCount || address.Level >= m_Levels.size() || page.Written != (address.Level > 0) ||
            address.X >= m_Levels[address.Level].PagesX || address.Z >= m_Levels[address.Level].PagesZ ||
            page.Deltas.size() != kPageSampleCount)
        {
            Clear();
            return false;
        }
        (page.Written ? m_Written : m_Pages).InsertOrAssign(address, page.Deltas);
        if (page.Written)
            ++m_WrittenPages[address.Level];
        IncludePageFaceRect(m_Extents, address.Face,
                            Level0Of(OwnedRect(address, m_Levels[address.Level]), address.Level,
                                     m_Levels.front().SamplesX, m_Levels.front().SamplesZ));
    }
    // Each level from the one below and its own writes; a sample's value does not depend on the rect
    // that composes it, so overlapping rects agree.
    std::vector<FaceRect> rects;
    for (uint32 level = 0; level < m_Levels.size(); ++level)
    {
        rects.clear();
        if (level == 0)
        {
            for (const auto& [address, deltas] : m_Pages)
                rects.push_back(FaceRect{address.Face, OwnedRect(address, m_Levels[0])});
        }
        else
        {
            const PageStoreLevel& shape = m_Levels[level];
            for (const auto& [address, deltas] : m_Pages)
                if (address.Level == level - 1u)
                    rects.push_back(FaceRect{
                        address.Face,
                        CoarserRect(OwnedRect(address, m_Levels[level - 1u]), shape.SamplesX, shape.SamplesZ)});
            for (const auto& [address, deltas] : m_Written)
                if (address.Level == level)
                    rects.push_back(FaceRect{address.Face, OwnedRect(address, shape)});
            for (const FaceRect& rect : rects)
                ComposeLevel(rect.Face, level, rect.Rect);
        }
        for (const FaceRect& rect : rects)
            RefreshAprons(rect.Face, level, rect.Rect);
    }
    DropZeroPages();
    return true;
}

void HeightPageLayer::Clear()
{
    m_Pages.Clear();
    m_Written.Clear();
    std::ranges::fill(m_WrittenPages, 0u);
    m_Extents = {};
    m_Touched.clear();
    m_TouchedWritten.clear();
}

bool HeightPageLayer::FitsLevels(std::span<const PageStoreLevel> levels) const
{
    return levels.size() == m_Levels.size() && !levels.empty() &&
           levels.front().SamplesX == m_Levels.front().SamplesX && levels.front().SamplesZ == m_Levels.front().SamplesZ;
}

uint64 HeightPageLayer::Bytes() const
{
    return static_cast<uint64>(m_Pages.Size()) * kPageSampleCount * sizeof(float32);
}

PageFaceSample HeightPageLayer::StoredSample(uint8 face, uint32 level, int64 x, int64 z) const
{
    const PageStoreLevel& shape = m_Levels[level];
    if (m_FaceCount == kPageStoreMaxFaces)
        return PageCubeSampleAt(face, x, z, shape.SamplesX);
    return PageFaceSample{face, ClampIndex(x, shape.SamplesX), ClampIndex(z, shape.SamplesZ)};
}

PageFaceSample HeightPageLayer::WindowSample(uint8 face, uint32 level, int64 x, int64 z, uint32 margin) const
{
    const PageStoreLevel& shape = m_Levels[level];
    if (m_FaceCount == kPageStoreMaxFaces)
        return PageCubeWindowSampleAt(face, x, z, shape.SamplesX, margin);
    return PageFaceSample{face, ClampIndex(x, shape.SamplesX), ClampIndex(z, shape.SamplesZ)};
}

void HeightPageLayer::AddPastSide(const PageAddress& address, uint32 margin, PageFaceSide side,
                                  std::span<float32> window) const
{
    const uint32 stride = kPageOwnedSamples + 2u * margin;
    const std::optional<PageSampleRect> strip = PastSideStrip(address, m_Levels[address.Level], margin, side);
    if (!strip)
        return;
    uint8 neighbor = address.Face;
    const PageSampleRect rect = PastSideRect(address, margin, *strip, neighbor);
    if (neighbor == address.Face)
        return;
    Memory::ScratchScope scratch;
    const std::span<float32> term = scratch.AllocateArray<float32>(static_cast<std::size_t>(rect.Width()) * rect.Height());
    GatherInto(m_Pages, neighbor, address.Level, rect, term);
    AddProlongedWrites(neighbor, address.Level, rect, term);
    const auto [firstX, firstZ] = WindowOrigin(address, margin);
    for (uint32 row = strip->MinZ; row <= strip->MaxZ; ++row)
        for (uint32 column = strip->MinX; column <= strip->MaxX; ++column)
        {
            const PageFaceSample source = WindowSample(address.Face, address.Level, firstX + column, firstZ + row, margin);
            window[static_cast<std::size_t>(row) * stride + column] =
                term[static_cast<std::size_t>(source.Z - rect.MinZ) * rect.Width() + (source.X - rect.MinX)];
        }
}

PageSampleRect HeightPageLayer::PastSideRect(const PageAddress& address, uint32 margin, const PageSampleRect& strip,
                                             uint8& neighbor) const
{
    const auto [firstX, firstZ] = WindowOrigin(address, margin);
    PageSampleRect rect{std::numeric_limits<uint32>::max(), std::numeric_limits<uint32>::max(), 0u, 0u};
    for (uint32 row = strip.MinZ; row <= strip.MaxZ; ++row)
        for (uint32 column = strip.MinX; column <= strip.MaxX; ++column)
        {
            const PageFaceSample source = WindowSample(address.Face, address.Level, firstX + column, firstZ + row, margin);
            neighbor = source.Face;
            rect = Union(rect, PageSampleRect{source.X, source.Z, source.X, source.Z});
        }
    return rect;
}

bool HeightPageLayer::WindowReached(const PageAddress& address, uint32 margin) const
{
    // A layer that holds no page and no write reaches no window: skip the strip walks past the sides.
    if (m_Pages.Empty() && std::ranges::all_of(m_WrittenPages, [](uint32 count) { return count == 0u; }))
        return false;
    if (RectReached(address.Face, address.Level, WindowInFace(address, m_Levels[address.Level], margin)))
        return true;
    if (m_FaceCount != kPageStoreMaxFaces)
        return false;
    for (PageFaceSide side : {PageFaceSide::MinZ, PageFaceSide::MaxZ, PageFaceSide::MinX, PageFaceSide::MaxX})
    {
        const std::optional<PageSampleRect> strip = PastSideStrip(address, m_Levels[address.Level], margin, side);
        if (!strip)
            continue;
        uint8 neighbor = address.Face;
        const PageSampleRect rect = PastSideRect(address, margin, *strip, neighbor);
        if (neighbor != address.Face && RectReached(neighbor, address.Level, rect))
            return true;
    }
    return false;
}

bool HeightPageLayer::RectReached(uint8 face, uint32 level, const PageSampleRect& rect) const
{
    for (uint32 pz = rect.MinZ / kPageOwnedSamples; pz <= rect.MaxZ / kPageOwnedSamples; ++pz)
        for (uint32 px = rect.MinX / kPageOwnedSamples; px <= rect.MaxX / kPageOwnedSamples; ++px)
            if (m_Pages.Find(PageAddress{face, static_cast<uint8>(level), px, pz}))
                return true;
    return ProlongedWritesReach(face, level, rect);
}

uint32 HeightPageLayer::Gather(const PageMap& pages, uint8 face, uint32 level, const PageSampleRect& rect,
                               std::vector<float32>& out)
{
    out.assign(static_cast<std::size_t>(rect.Width()) * rect.Height(), 0.0f);
    return GatherInto(pages, face, level, rect, out);
}

uint32 HeightPageLayer::GatherInto(const PageMap& pages, uint8 face, uint32 level, const PageSampleRect& rect,
                                   std::span<float32> out)
{
    assert(out.size() == static_cast<std::size_t>(rect.Width()) * rect.Height());
    uint32 searched = 0;
    for (uint32 pz = rect.MinZ / kPageOwnedSamples; pz <= rect.MaxZ / kPageOwnedSamples; ++pz)
        for (uint32 px = rect.MinX / kPageOwnedSamples; px <= rect.MaxX / kPageOwnedSamples; ++px)
        {
            ++searched;
            const std::vector<float32>* page = pages.Find(PageAddress{face, static_cast<uint8>(level), px, pz});
            if (!page)
                continue;
            const uint32 x0 = std::max(rect.MinX, px * kPageOwnedSamples);
            const uint32 x1 = std::min(rect.MaxX, px * kPageOwnedSamples + kPageOwnedSamples - 1u);
            const uint32 z0 = std::max(rect.MinZ, pz * kPageOwnedSamples);
            const uint32 z1 = std::min(rect.MaxZ, pz * kPageOwnedSamples + kPageOwnedSamples - 1u);
            for (uint32 z = z0; z <= z1; ++z)
            {
                const float32* source = page->data() + StrideIndex(x0 - px * kPageOwnedSamples + kPageApronSamples,
                                                                   z - pz * kPageOwnedSamples + kPageApronSamples);
                std::copy(source, source + (x1 - x0 + 1u),
                          out.data() + static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x0 - rect.MinX));
            }
        }
    return searched;
}

void HeightPageLayer::Scatter(bool written, uint8 face, uint32 level, const PageSampleRect& rect,
                              std::span<const float32> values)
{
    PageMap& pages = written ? m_Written : m_Pages;
    for (uint32 pz = rect.MinZ / kPageOwnedSamples; pz <= rect.MaxZ / kPageOwnedSamples; ++pz)
        for (uint32 px = rect.MinX / kPageOwnedSamples; px <= rect.MaxX / kPageOwnedSamples; ++px)
        {
            const PageAddress address{face, static_cast<uint8>(level), px, pz};
            const uint32 x0 = std::max(rect.MinX, px * kPageOwnedSamples);
            const uint32 x1 = std::min(rect.MaxX, px * kPageOwnedSamples + kPageOwnedSamples - 1u);
            const uint32 z0 = std::max(rect.MinZ, pz * kPageOwnedSamples);
            const uint32 z1 = std::min(rect.MaxZ, pz * kPageOwnedSamples + kPageOwnedSamples - 1u);
            const auto row = [&values, &rect, x0, x1](uint32 z) {
                return values.subspan(static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x0 - rect.MinX),
                                      x1 - x0 + 1u);
            };
            std::vector<float32>* page = pages.Find(address);
            if (!page)
            {
                bool any = false;
                for (uint32 z = z0; z <= z1 && !any; ++z)
                    any = std::ranges::any_of(row(z), [](float32 v) { return v != 0.0f; });
                if (!any)
                    continue; // zeros into a page the layer does not hold
                KeepForCapture(written, address);
                page = &pages.InsertOrAssign(address, std::vector<float32>(kPageSampleCount, 0.0f));
                if (written)
                    ++m_WrittenPages[level];
            }
            else
            {
                KeepForCapture(written, address);
            }
            for (uint32 z = z0; z <= z1; ++z)
                std::ranges::copy(row(z), page->data() + StrideIndex(x0 - px * kPageOwnedSamples + kPageApronSamples,
                                                                     z - pz * kPageOwnedSamples + kPageApronSamples));
            (written ? m_TouchedWritten : m_Touched).push_back(address);
        }
}

PageSampleRect HeightPageLayer::FilterLevel(uint8 face, uint32 level, const PageSampleRect& finerReach)
{
    const PageStoreLevel& coarser = m_Levels[level];
    const PageSampleRect rect = CoarserRect(finerReach, coarser.SamplesX, coarser.SamplesZ);
    ComposeLevel(face, level, rect);
    return rect;
}

void HeightPageLayer::ComposeLevel(uint8 face, uint32 level, const PageSampleRect& rect)
{
    const PageStoreLevel& finer = m_Levels[level - 1u];
    const PageSampleRect source{ClampIndex(2 * static_cast<int64>(rect.MinX) - 1, finer.SamplesX),
                                ClampIndex(2 * static_cast<int64>(rect.MinZ) - 1, finer.SamplesZ),
                                ClampIndex(2 * static_cast<int64>(rect.MaxX) + 1, finer.SamplesX),
                                ClampIndex(2 * static_cast<int64>(rect.MaxZ) + 1, finer.SamplesZ)};
    Gather(m_Pages, face, level - 1u, source, m_Finer);
    const auto tap = [this, &source, &finer](int64 x, uint32 z) {
        return m_Finer[static_cast<std::size_t>(z - source.MinZ) * source.Width() +
                       (ClampIndex(x, finer.SamplesX) - source.MinX)];
    };
    m_Coarser.resize(static_cast<std::size_t>(rect.Width()) * rect.Height());
    for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
        for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
        {
            float32 sum = 0.0f;
            for (int32 dz = -1; dz <= 1; ++dz)
            {
                const uint32 fz = ClampIndex(2 * static_cast<int64>(z) + dz, finer.SamplesZ);
                const float32 wz = dz == 0 ? 0.5f : 0.25f;
                const float32 row = 0.25f * tap(2 * static_cast<int64>(x) - 1, fz) +
                                    0.5f * tap(2 * static_cast<int64>(x), fz) +
                                    0.25f * tap(2 * static_cast<int64>(x) + 1, fz);
                sum += wz * row;
            }
            m_Coarser[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)] = sum;
        }
    if (m_WrittenPages[level] > 0u)
    {
        Gather(m_Written, face, level, rect, m_Finer);
        for (std::size_t i = 0; i < m_Coarser.size(); ++i)
            m_Coarser[i] += m_Finer[i];
    }
    Scatter(false, face, level, rect, m_Coarser);
}

void HeightPageLayer::RefreshAprons(uint8 face, uint32 level, const PageSampleRect& reach)
{
    const PageStoreLevel& shape = m_Levels[level];
    uint32 minX = 0, maxX = 0, minZ = 0, maxZ = 0;
    PageSpan(reach.MinX, reach.MaxX, shape.PagesX, minX, maxX);
    PageSpan(reach.MinZ, reach.MaxZ, shape.PagesZ, minZ, maxZ);
    const bool cube = m_FaceCount == kPageStoreMaxFaces;
    for (uint32 z = minZ; z <= maxZ; ++z)
        for (uint32 x = minX; x <= maxX; ++x)
        {
            const PageAddress address{face, static_cast<uint8>(level), x, z};
            if (ApronReadsRect(address, shape, cube, reach))
                RefreshPageApron(address);
        }
    if (!cube)
        return;
    // A neighbor face's pages past a shared side store this face's samples one in from it.
    const uint32 last = shape.SamplesX - 1u;
    for (const SideReach& side : SidesNear(reach, shape.SamplesX, kPageApronSamples))
    {
        if (!side.Near)
            continue;
        const PageFaceEdge edge = PageFaceNeighbor(face, side.Side);
        uint32 neighborFirst = 0, neighborLast = 0;
        NeighborAlong(edge, side.AlongMin, side.AlongMax, shape.SamplesX, neighborFirst, neighborLast);
        uint32 alongMin = 0, alongMax = 0;
        PageSpan(neighborFirst, neighborLast, shape.PagesX, alongMin, alongMax);
        const bool minSide = edge.Side == PageFaceSide::MinX || edge.Side == PageFaceSide::MinZ;
        for (uint32 across = 0; across < shape.PagesX; ++across)
        {
            // Page p stores samples 128p - 1 to 128p + 128: only a page past the side stores the apron there.
            if (minSide ? across != 0u : across * kPageOwnedSamples + kPageOwnedSamples <= last)
                continue;
            for (uint32 along = alongMin; along <= alongMax; ++along)
                RefreshPageApron(IsColumnSide(edge.Side) ? PageAddress{edge.Face, static_cast<uint8>(level), across, along}
                                                         : PageAddress{edge.Face, static_cast<uint8>(level), along, across});
        }
    }
}

void HeightPageLayer::RefreshPageApron(const PageAddress& address)
{
    // Every stored sample the page does not own is read again from the pages that own it, a line at a
    // time: the ring inside the level from this face; past a side, the line ReadPastSide gives, the
    // same sample out to the page's edge; past two sides, the corner sample.
    const PageStoreLevel& shape = m_Levels[address.Level];
    const uint8 level = address.Level;
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples;
    const int64 endX = firstX + kPageStrideSamples - 1;
    const int64 endZ = firstZ + kPageStrideSamples - 1;
    const int64 lastX = static_cast<int64>(shape.SamplesX) - 1;
    const int64 lastZ = static_cast<int64>(shape.SamplesZ) - 1;
    const PageSampleRect stored = StoredRect(address, shape);
    std::vector<float32>* page = m_Pages.Find(address);
    ++m_ApronWork.Pages;
    const auto strideX = [firstX](int64 x) { return static_cast<uint32>(x - firstX); };
    const auto strideZ = [firstZ](int64 z) { return static_cast<uint32>(z - firstZ); };

    // The ring inside the level: the first and last stored rows, then the first and last stored columns
    // between them.
    for (const int64 z : {firstZ, endZ})
    {
        if (z < 0 || z > lastZ)
            continue;
        const PageSampleRect row{stored.MinX, static_cast<uint32>(z), stored.MaxX, static_cast<uint32>(z)};
        m_ApronWork.Lookups += Gather(m_Pages, address.Face, level, row, m_Line);
        for (uint32 x = row.MinX; x <= row.MaxX; ++x)
            StoreApronSample(address, page, StrideIndex(strideX(x), strideZ(z)), m_Line[x - row.MinX]);
    }
    const uint32 innerMinZ = static_cast<uint32>(firstZ + 1);
    const uint32 innerMaxZ = static_cast<uint32>(std::min(endZ - 1, lastZ));
    for (const int64 x : {firstX, endX})
    {
        if (x < 0 || x > lastX)
            continue;
        const PageSampleRect column{static_cast<uint32>(x), innerMinZ, static_cast<uint32>(x), innerMaxZ};
        m_ApronWork.Lookups += Gather(m_Pages, address.Face, level, column, m_Line);
        for (uint32 z = innerMinZ; z <= innerMaxZ; ++z)
            StoreApronSample(address, page, StrideIndex(strideX(x), strideZ(z)), m_Line[z - innerMinZ]);
    }

    // Past a side: one sample a row (or column) inside the level, the same for every stride past the side.
    if (firstX < 0 || endX > lastX)
        for (const bool minSide : {true, false})
        {
            if (minSide ? firstX >= 0 : endX <= lastX)
                continue;
            ReadPastSide(address.Face, level, minSide ? PageFaceSide::MinX : PageFaceSide::MaxX, stored.MinZ,
                         stored.MaxZ, m_Line);
            const uint32 fromStride = minSide ? 0u : strideX(lastX + 1);
            const uint32 toStride = minSide ? strideX(-1) : kPageStrideSamples - 1u;
            for (uint32 z = stored.MinZ; z <= stored.MaxZ; ++z)
                for (uint32 stride = fromStride; stride <= toStride; ++stride)
                    StoreApronSample(address, page, StrideIndex(stride, strideZ(z)), m_Line[z - stored.MinZ]);
        }
    if (firstZ < 0 || endZ > lastZ)
        for (const bool minSide : {true, false})
        {
            if (minSide ? firstZ >= 0 : endZ <= lastZ)
                continue;
            ReadPastSide(address.Face, level, minSide ? PageFaceSide::MinZ : PageFaceSide::MaxZ, stored.MinX,
                         stored.MaxX, m_Line);
            const uint32 fromStride = minSide ? 0u : strideZ(lastZ + 1);
            const uint32 toStride = minSide ? strideZ(-1) : kPageStrideSamples - 1u;
            for (uint32 stride = fromStride; stride <= toStride; ++stride)
                for (uint32 x = stored.MinX; x <= stored.MaxX; ++x)
                    StoreApronSample(address, page, StrideIndex(strideX(x), stride), m_Line[x - stored.MinX]);
        }

    // Past two sides: the level's corner sample.
    for (const bool minX : {true, false})
        for (const bool minZ : {true, false})
        {
            if ((minX ? firstX >= 0 : endX <= lastX) || (minZ ? firstZ >= 0 : endZ <= lastZ))
                continue;
            const uint32 cornerX = minX ? 0u : static_cast<uint32>(lastX);
            const uint32 cornerZ = minZ ? 0u : static_cast<uint32>(lastZ);
            m_ApronWork.Lookups +=
                Gather(m_Pages, address.Face, level, PageSampleRect{cornerX, cornerZ, cornerX, cornerZ}, m_Line);
            const uint32 fromX = minX ? 0u : strideX(lastX + 1);
            const uint32 toX = minX ? strideX(-1) : kPageStrideSamples - 1u;
            const uint32 fromZ = minZ ? 0u : strideZ(lastZ + 1);
            const uint32 toZ = minZ ? strideZ(-1) : kPageStrideSamples - 1u;
            for (uint32 z = fromZ; z <= toZ; ++z)
                for (uint32 x = fromX; x <= toX; ++x)
                    StoreApronSample(address, page, StrideIndex(x, z), m_Line.front());
        }
    if (page)
        m_Touched.push_back(address);
}

void HeightPageLayer::ReadPastSide(uint8 face, uint32 level, PageFaceSide side, uint32 alongMin, uint32 alongMax,
                                   std::vector<float32>& out)
{
    const PageStoreLevel& shape = m_Levels[level];
    if (m_FaceCount != kPageStoreMaxFaces)
    {
        const bool minSide = side == PageFaceSide::MinX || side == PageFaceSide::MinZ;
        const uint32 across = minSide ? 0u : (IsColumnSide(side) ? shape.SamplesX : shape.SamplesZ) - 1u;
        const PageSampleRect edge = IsColumnSide(side) ? PageSampleRect{across, alongMin, across, alongMax}
                                                       : PageSampleRect{alongMin, across, alongMax, across};
        m_ApronWork.Lookups += Gather(m_Pages, face, level, edge, out);
        return;
    }
    const PageFaceEdge edge = PageFaceNeighbor(face, side);
    uint32 first = 0, last = 0;
    NeighborAlong(edge, alongMin, alongMax, shape.SamplesX, first, last);
    const uint32 across =
        edge.Side == PageFaceSide::MinX || edge.Side == PageFaceSide::MinZ ? 1u : shape.SamplesX - 2u;
    const PageSampleRect line = IsColumnSide(edge.Side) ? PageSampleRect{across, first, across, last}
                                                        : PageSampleRect{first, across, last, across};
    m_ApronWork.Lookups += Gather(m_Pages, edge.Face, level, line, out);
    if (edge.Reversed)
        std::reverse(out.begin(), out.end());
}

void HeightPageLayer::StoreApronSample(const PageAddress& address, std::vector<float32>*& page, std::size_t index,
                                       float32 value)
{
    if (!page)
    {
        if (value == 0.0f)
            return;
        KeepForCapture(false, address);
        page = &m_Pages.InsertOrAssign(address, std::vector<float32>(kPageSampleCount, 0.0f));
    }
    float32& held = (*page)[index];
    if (held == value)
        return;
    KeepForCapture(false, address);
    held = value;
}

void HeightPageLayer::DropZeroPages()
{
    for (const bool written : {false, true})
    {
        std::vector<PageAddress>& touched = written ? m_TouchedWritten : m_Touched;
        PageMap& pages = written ? m_Written : m_Pages;
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        for (const PageAddress& address : touched)
        {
            const std::vector<float32>* page = pages.Find(address);
            if (!page || !std::ranges::all_of(*page, [](float32 v) { return v == 0.0f; }))
                continue;
            KeepForCapture(written, address);
            pages.Erase(address);
            if (written)
                --m_WrittenPages[address.Level];
        }
        touched.clear();
    }
}

void HeightPageLayer::AddToRect(bool written, uint8 face, uint32 level, const PageSampleRect& rect,
                                std::span<const float32> deltas)
{
    Gather(written ? m_Written : m_Pages, face, level, rect, m_Finer);
    for (std::size_t i = 0; i < m_Finer.size(); ++i)
        m_Finer[i] += deltas[i];
    Scatter(written, face, level, rect, m_Finer);
}

bool HeightPageLayer::ProlongedWritesReach(uint8 face, uint32 level, const PageSampleRect& rect) const
{
    PageSampleRect reach = rect;
    for (uint32 coarser = level + 1u; coarser < m_Levels.size(); ++coarser)
    {
        const PageStoreLevel& shape = m_Levels[coarser];
        reach = ProlongationSource(reach, shape.SamplesX, shape.SamplesZ);
        if (m_WrittenPages[coarser] == 0u)
            continue;
        for (uint32 pz = reach.MinZ / kPageOwnedSamples; pz <= reach.MaxZ / kPageOwnedSamples; ++pz)
            for (uint32 px = reach.MinX / kPageOwnedSamples; px <= reach.MaxX / kPageOwnedSamples; ++px)
                if (m_Written.Find(PageAddress{face, static_cast<uint8>(coarser), px, pz}))
                    return true;
    }
    return false;
}

void HeightPageLayer::AddProlongedWrites(uint8 face, uint32 level, const PageSampleRect& rect,
                                         std::span<float32> out) const
{
    const uint32 coarser = level + 1u;
    if (coarser >= m_Levels.size() || !ProlongedWritesReach(face, level, rect))
        return;
    const PageStoreLevel& shape = m_Levels[coarser];
    const PageSampleRect source = ProlongationSource(rect, shape.SamplesX, shape.SamplesZ);
    std::vector<float32> coarse;
    Gather(m_Written, face, coarser, source, coarse);
    AddProlongedWrites(face, coarser, source, coarse);
    // Sample x of the finer level sits on coarser sample x / 2 when x is even, halfway to the next when odd.
    const auto at = [&coarse, &source](uint32 x, uint32 z) {
        return coarse[static_cast<std::size_t>(std::min(z, source.MaxZ) - source.MinZ) * source.Width() +
                      (std::min(x, source.MaxX) - source.MinX)];
    };
    for (uint32 z = rect.MinZ; z <= rect.MaxZ; ++z)
        for (uint32 x = rect.MinX; x <= rect.MaxX; ++x)
        {
            const uint32 x0 = x / 2u;
            const uint32 x1 = (x + 1u) / 2u;
            const uint32 z0 = z / 2u;
            const uint32 z1 = (z + 1u) / 2u;
            const float32 row0 = 0.5f * (at(x0, z0) + at(x1, z0));
            const float32 row1 = 0.5f * (at(x0, z1) + at(x1, z1));
            out[static_cast<std::size_t>(z - rect.MinZ) * rect.Width() + (x - rect.MinX)] += 0.5f * (row0 + row1);
        }
}

void HeightPageLayer::AddProlongedPage(const PageAddress& address, std::span<float32> samples) const
{
    // The page's samples inside its face read one rect; a stored sample past a side (the apron) reads the
    // sample it stores (StoredSample) on its own.
    const PageSampleRect inFace = StoredRect(address, m_Levels[address.Level]);
    const int64 firstX = int64{address.X} * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = int64{address.Z} * kPageOwnedSamples - kPageApronSamples;
    std::vector<float32> term(static_cast<std::size_t>(inFace.Width()) * inFace.Height(), 0.0f);
    AddProlongedWrites(address.Face, address.Level, inFace, term);
    for (uint32 strideZ = 0; strideZ < kPageStrideSamples; ++strideZ)
        for (uint32 strideX = 0; strideX < kPageStrideSamples; ++strideX)
        {
            const int64 x = firstX + strideX;
            const int64 z = firstZ + strideZ;
            float32 value = 0.0f;
            if (x >= inFace.MinX && x <= inFace.MaxX && z >= inFace.MinZ && z <= inFace.MaxZ)
            {
                value = term[static_cast<std::size_t>(z - inFace.MinZ) * inFace.Width() +
                             static_cast<std::size_t>(x - inFace.MinX)];
            }
            else
            {
                const PageFaceSample source = StoredSample(address.Face, address.Level, x, z);
                AddProlongedWrites(source.Face, address.Level, PageSampleRect{source.X, source.Z, source.X, source.Z},
                                   std::span<float32>(&value, 1));
            }
            samples[StrideIndex(strideX, strideZ)] += value;
        }
}

void HeightPageLayer::KeepForCapture(bool written, const PageAddress& address)
{
    if (m_Capturing)
        KeepPage(written ? m_CapturedWritten : m_CapturedPages, written ? m_Written : m_Pages, address);
}

namespace
{

void RefreshLayerRect(PageResidencyManager& residency, uint32 stream, const HeightPageLayer& layer, uint8 face,
                      const PageSampleRect& rect, uint32 marginSamples, std::vector<PageFaceRect>& scratch)
{
    // A write near a planet's face side also changes the neighbor face's pages past that side.
    layer.RefreshRects(face, rect, marginSamples, scratch);
    for (const PageFaceRect& faceRect : scratch)
        residency.Refresh(stream, faceRect.Face, faceRect.Level0Rect);
}

} // namespace

void RefreshLayeredPages(PageResidencyManager& residency, uint32 stream, std::span<const HeightPageLayer* const> held,
                         uint64 heldVersion, std::span<const HeightPageLayer* const> requested,
                         const PageFaceRects& changed, uint64 changedSince, uint32 marginSamples,
                         std::vector<PageFaceRect>& scratch)
{
    // An edit: the pages over the changed rects are rewritten with the new layers as they reload,
    // keeping their old content until then.
    if (changedSince == heldVersion)
    {
        // Any layer of the stream gives the neighbor faces' edge bands: they depend on its pyramid alone.
        const HeightPageLayer* layer = nullptr;
        for (std::span<const HeightPageLayer* const> layers : {requested, held})
            for (const HeightPageLayer* candidate : layers)
                if (!layer && candidate)
                    layer = candidate;
        for (uint8 face = 0; face < changed.size(); ++face)
        {
            if (!changed[face])
                continue;
            if (layer)
                RefreshLayerRect(residency, stream, *layer, face, *changed[face], marginSamples, scratch);
            else
                residency.Refresh(stream, face, *changed[face]);
        }
        return;
    }
    for (std::span<const HeightPageLayer* const> layers : {held, requested})
        for (const HeightPageLayer* layer : layers)
            for (uint8 face = 0; layer && face < layer->FaceCount(); ++face)
                if (const std::optional<PageSampleRect>& extent = layer->Extent(face))
                    RefreshLayerRect(residency, stream, *layer, face, *extent, marginSamples, scratch);
}

} // namespace GameEngine::PageStreaming
