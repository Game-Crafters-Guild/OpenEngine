#include "PageStreaming/Height/HeightPageLayer.h"

#include <algorithm>
#include <cassert>

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

// True when stride position `stride` of a page holds a sample the page owns: inside its apron, and
// `sample`, the index on the level, is below the level's `samples`.
bool IsOwnedStride(uint32 stride, int64 sample, uint32 samples)
{
    return stride >= kPageApronSamples && stride <= kPageOwnedSamples && sample < static_cast<int64>(samples);
}

// The side of a cube face and the samples along it that a rect within one sample of the side holds.
struct SideReach
{
    bool Near = false;
    PageFaceSide Side = PageFaceSide::MinZ;
    uint32 AlongMin = 0;
    uint32 AlongMax = 0;
};

// The sides of a face of `samples` samples whose neighbor's apron reads a sample of `rect`: a page
// past a side stores the sample one in from it.
std::array<SideReach, kPageFaceSides> SidesNear(const PageSampleRect& rect, uint32 samples)
{
    const uint32 inner = samples - 2u;
    return {SideReach{rect.MinZ <= 1u, PageFaceSide::MinZ, rect.MinX, rect.MaxX},
            SideReach{rect.MaxZ >= inner, PageFaceSide::MaxZ, rect.MinX, rect.MaxX},
            SideReach{rect.MinX <= 1u, PageFaceSide::MinX, rect.MinZ, rect.MaxZ},
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

} // namespace

bool PageReadsLevel0Rect(const PageAddress& address, const PageSampleRect& level0Rect)
{
    // The page stores its level's samples 128p - 1 to 128p + 128; level-L sample s sits at level-0
    // sample s * 2^L, and a coarser sample is filtered from the level-0 samples within 2^L - 1 of it.
    const int64 reach = (int64{1} << address.Level) - 1;
    const int64 firstX = ((static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples) << address.Level) - reach;
    const int64 firstZ = ((static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples) << address.Level) - reach;
    const int64 lastX = ((static_cast<int64>(address.X) * kPageOwnedSamples + kPageOwnedSamples) << address.Level) + reach;
    const int64 lastZ = ((static_cast<int64>(address.Z) * kPageOwnedSamples + kPageOwnedSamples) << address.Level) + reach;
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
    Scatter(face, 0, rect, deltas);
    RefreshAprons(face, 0, rect);
    PageSampleRect reach = rect;
    for (uint32 level = 1; level < m_Levels.size(); ++level)
    {
        reach = FilterLevel(face, level, reach);
        RefreshAprons(face, level, reach);
    }
    DropZeroPages();
    m_Extents[face] = m_Extents[face] ? Union(*m_Extents[face], rect) : rect;
}

bool HeightPageLayer::ApplyToPage(const PageAddress& address, std::span<float32> samples) const
{
    const std::vector<float32>* page = m_Pages.Find(address);
    if (!page || samples.size() != kPageSampleCount)
        return false;
    for (std::size_t i = 0; i < kPageSampleCount; ++i)
        samples[i] += (*page)[i];
    return true;
}

std::span<const float32> HeightPageLayer::PageDeltas(const PageAddress& address) const
{
    const std::vector<float32>* page = m_Pages.Find(address);
    return page ? std::span<const float32>(*page) : std::span<const float32>();
}

void HeightPageLayer::RefreshRects(uint8 face, const PageSampleRect& rect, std::vector<PageFaceRect>& out) const
{
    out.clear();
    out.push_back(PageFaceRect{face, rect});
    if (m_FaceCount != kPageStoreMaxFaces || m_Levels.empty())
        return;
    // Level L's reach on a side changes the neighbor's level-L pages past that side; their level-0
    // band is the side's edge sample over the reach scaled to level 0.
    const uint32 samples0 = m_Levels.front().SamplesX;
    std::array<std::optional<PageFaceRect>, kPageFaceSides> bands;
    for (uint32 level = 0; level < m_Levels.size(); ++level)
    {
        const uint32 samples = m_Levels[level].SamplesX;
        const std::array<SideReach, kPageFaceSides> sides = SidesNear(Level0RectReach(m_Levels, rect, level), samples);
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

bool HeightPageLayer::FitsLevels(std::span<const PageStoreLevel> levels) const
{
    return levels.size() == m_Levels.size() && !levels.empty() &&
           levels.front().SamplesX == m_Levels.front().SamplesX && levels.front().SamplesZ == m_Levels.front().SamplesZ;
}

uint64 HeightPageLayer::Bytes() const
{
    return static_cast<uint64>(m_Pages.Size()) * kPageSampleCount * sizeof(float32);
}

float32 HeightPageLayer::Sample(uint8 face, uint32 level, uint32 x, uint32 z) const
{
    const PageAddress address{face, static_cast<uint8>(level), x / kPageOwnedSamples, z / kPageOwnedSamples};
    const std::vector<float32>* page = m_Pages.Find(address);
    if (!page)
        return 0.0f;
    return (*page)[StrideIndex(x % kPageOwnedSamples + kPageApronSamples, z % kPageOwnedSamples + kPageApronSamples)];
}

PageFaceSample HeightPageLayer::StoredSample(uint8 face, uint32 level, int64 x, int64 z) const
{
    const PageStoreLevel& shape = m_Levels[level];
    if (m_FaceCount == kPageStoreMaxFaces)
        return PageCubeSampleAt(face, x, z, shape.SamplesX);
    return PageFaceSample{face, ClampIndex(x, shape.SamplesX), ClampIndex(z, shape.SamplesZ)};
}

void HeightPageLayer::Gather(uint8 face, uint32 level, const PageSampleRect& rect, std::vector<float32>& out) const
{
    out.assign(static_cast<std::size_t>(rect.Width()) * rect.Height(), 0.0f);
    for (uint32 pz = rect.MinZ / kPageOwnedSamples; pz <= rect.MaxZ / kPageOwnedSamples; ++pz)
        for (uint32 px = rect.MinX / kPageOwnedSamples; px <= rect.MaxX / kPageOwnedSamples; ++px)
        {
            const std::vector<float32>* page = m_Pages.Find(PageAddress{face, static_cast<uint8>(level), px, pz});
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
}

void HeightPageLayer::Scatter(uint8 face, uint32 level, const PageSampleRect& rect, std::span<const float32> values)
{
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
            std::vector<float32>* page = m_Pages.Find(address);
            if (!page)
            {
                bool any = false;
                for (uint32 z = z0; z <= z1 && !any; ++z)
                    any = std::ranges::any_of(row(z), [](float32 v) { return v != 0.0f; });
                if (!any)
                    continue; // zeros into a page the layer does not hold
                page = &m_Pages.InsertOrAssign(address, std::vector<float32>(kPageSampleCount, 0.0f));
            }
            for (uint32 z = z0; z <= z1; ++z)
                std::ranges::copy(row(z), page->data() + StrideIndex(x0 - px * kPageOwnedSamples + kPageApronSamples,
                                                                     z - pz * kPageOwnedSamples + kPageApronSamples));
            m_Touched.push_back(address);
        }
}

PageSampleRect HeightPageLayer::FilterLevel(uint8 face, uint32 level, const PageSampleRect& finerReach)
{
    const PageStoreLevel& finer = m_Levels[level - 1u];
    const PageStoreLevel& coarser = m_Levels[level];
    const PageSampleRect rect = CoarserRect(finerReach, coarser.SamplesX, coarser.SamplesZ);
    const PageSampleRect source{ClampIndex(2 * static_cast<int64>(rect.MinX) - 1, finer.SamplesX),
                                ClampIndex(2 * static_cast<int64>(rect.MinZ) - 1, finer.SamplesZ),
                                ClampIndex(2 * static_cast<int64>(rect.MaxX) + 1, finer.SamplesX),
                                ClampIndex(2 * static_cast<int64>(rect.MaxZ) + 1, finer.SamplesZ)};
    Gather(face, level - 1u, source, m_Finer);
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
    Scatter(face, level, rect, m_Coarser);
    return rect;
}

void HeightPageLayer::RefreshAprons(uint8 face, uint32 level, const PageSampleRect& reach)
{
    const PageStoreLevel& shape = m_Levels[level];
    uint32 minX = 0, maxX = 0, minZ = 0, maxZ = 0;
    PageSpan(reach.MinX, reach.MaxX, shape.PagesX, minX, maxX);
    PageSpan(reach.MinZ, reach.MaxZ, shape.PagesZ, minZ, maxZ);
    for (uint32 z = minZ; z <= maxZ; ++z)
        for (uint32 x = minX; x <= maxX; ++x)
            RefreshPageApron(PageAddress{face, static_cast<uint8>(level), x, z});
    if (m_FaceCount != kPageStoreMaxFaces)
        return;
    // A neighbor face's pages past a shared side store this face's samples one in from it.
    const uint32 last = shape.SamplesX - 1u;
    for (const SideReach& side : SidesNear(reach, shape.SamplesX))
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
    // Every stored sample the page does not own is read again from the page that owns it.
    const PageStoreLevel& shape = m_Levels[address.Level];
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples;
    std::vector<float32>* page = m_Pages.Find(address);
    for (uint32 strideZ = 0; strideZ < kPageStrideSamples; ++strideZ)
    {
        const bool ownedRow = IsOwnedStride(strideZ, firstZ + strideZ, shape.SamplesZ);
        for (uint32 strideX = 0; strideX < kPageStrideSamples; ++strideX)
        {
            if (ownedRow && IsOwnedStride(strideX, firstX + strideX, shape.SamplesX))
                continue;
            const PageFaceSample source =
                StoredSample(address.Face, address.Level, firstX + strideX, firstZ + strideZ);
            const float32 value = Sample(source.Face, address.Level, source.X, source.Z);
            if (!page)
            {
                if (value == 0.0f)
                    continue;
                page = &m_Pages.InsertOrAssign(address, std::vector<float32>(kPageSampleCount, 0.0f));
            }
            (*page)[StrideIndex(strideX, strideZ)] = value;
        }
    }
    if (page)
        m_Touched.push_back(address);
}

void HeightPageLayer::DropZeroPages()
{
    std::sort(m_Touched.begin(), m_Touched.end());
    m_Touched.erase(std::unique(m_Touched.begin(), m_Touched.end()), m_Touched.end());
    for (const PageAddress& address : m_Touched)
    {
        const std::vector<float32>* page = m_Pages.Find(address);
        if (page && std::ranges::all_of(*page, [](float32 v) { return v == 0.0f; }))
            m_Pages.Erase(address);
    }
    m_Touched.clear();
}

} // namespace GameEngine::PageStreaming
