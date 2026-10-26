#include "PageStreaming/PageStorePatcher.h"

#include "PageCodec.h"
#include "PageFieldFilter.h"
#include "PageStreaming/PageStoreWriter.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace GameEngine::PageStreaming
{
namespace
{

// An inclusive span of samples or pages along one axis.
struct Span
{
    uint32 First = 0;
    uint32 Last = 0;

    uint32 Count() const { return Last - First + 1u; }
};

// The pages of a level whose stored samples hold a sample in `samples`. Page p stores the samples
// 128p - apron to 128p + 127 + apron.
Span PagesHolding(Span samples, uint32 pages, uint32 apron)
{
    const int64 low = static_cast<int64>(samples.First) - (kPageOwnedSamples - 1u) - apron;
    const uint32 first = low <= 0 ? 0u : static_cast<uint32>((low + kPageOwnedSamples - 1u) / kPageOwnedSamples);
    const uint32 last = std::min((samples.Last + apron) / kPageOwnedSamples, pages - 1u);
    return {first, last};
}

// The coarser samples that read a finer sample in `finer`: coarser sample j reads 2j - 1 to 2j + 1.
Span CoarserSamples(Span finer, uint32 coarserSamples)
{
    return {finer.First / 2u, std::min((finer.Last + 1u) / 2u, coarserSamples - 1u)};
}

// The finer samples that the stored samples of coarser page `page` read, clamped to the finer level.
Span FinerSamplesRead(uint32 page, uint32 apron, uint32 finerSamples)
{
    const int64 first = static_cast<int64>(page) * kPageOwnedSamples - apron;
    const int64 last = static_cast<int64>(page) * kPageOwnedSamples + kPageOwnedSamples - 1 + apron;
    return {ClampIndex(2 * first - 1, finerSamples), ClampIndex(2 * last + 1, finerSamples)};
}

// The pages that own the samples of `samples`.
Span OwningPages(Span samples)
{
    return {samples.First / kPageOwnedSamples, samples.Last / kPageOwnedSamples};
}

Span Union(Span a, Span b)
{
    return {std::min(a.First, b.First), std::max(a.Last, b.Last)};
}

// The finer level's samples over a window, gathered from the owned samples of its stored pages.
struct FinerWindow
{
    Span X;
    Span Z;
    Span PagesX;
    Span PagesZ;
    uint32 TexelBytes = 0;
    std::vector<uint8> Texels;    // row-major over X by Z
    std::vector<uint8> PageReady; // one a page of PagesX by PagesZ: its samples are in Texels

    uint8* Texel(uint32 x, uint32 z)
    {
        return Texels.data() + (static_cast<std::size_t>(z - Z.First) * X.Count() + (x - X.First)) * TexelBytes;
    }
    const uint8* Texel(uint32 x, uint32 z) const { return const_cast<FinerWindow*>(this)->Texel(x, z); }

    bool Ready(Span pagesX, Span pagesZ) const
    {
        for (uint32 pz = pagesZ.First; pz <= pagesZ.Last; ++pz)
            for (uint32 px = pagesX.First; px <= pagesX.Last; ++px)
                if (PageReady[static_cast<std::size_t>(pz - PagesZ.First) * PagesX.Count() + (px - PagesX.First)] == 0)
                    return false;
        return true;
    }
};

// The samples page `address` owns, clamped to its level.
PageSampleRect OwnedSamples(const PageAddress& address, const PageStoreLevel& shape)
{
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples;
    return {ClampIndex(firstX, shape.SamplesX), ClampIndex(firstZ, shape.SamplesZ),
            ClampIndex(firstX + kPageOwnedSamples - 1, shape.SamplesX),
            ClampIndex(firstZ + kPageOwnedSamples - 1, shape.SamplesZ)};
}

// `samples` grown by `apron` on each side, clamped to the level.
PageSampleRect GrownSamples(const PageSampleRect& samples, uint32 apron, const PageStoreLevel& shape)
{
    return {ClampIndex(static_cast<int64>(samples.MinX) - apron, shape.SamplesX),
            ClampIndex(static_cast<int64>(samples.MinZ) - apron, shape.SamplesZ),
            ClampIndex(static_cast<int64>(samples.MaxX) + apron, shape.SamplesX),
            ClampIndex(static_cast<int64>(samples.MaxZ) + apron, shape.SamplesZ)};
}

// The samples page `address` stores, clamped to its level.
PageSampleRect StoredSamples(const PageAddress& address, uint32 apron, const PageStoreLevel& shape)
{
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - apron;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - apron;
    const int64 extent = kPageOwnedSamples - 1 + 2 * static_cast<int64>(apron);
    return {ClampIndex(firstX, shape.SamplesX), ClampIndex(firstZ, shape.SamplesZ),
            ClampIndex(firstX + extent, shape.SamplesX), ClampIndex(firstZ + extent, shape.SamplesZ)};
}

// The centered `toStride` square of a page's `fromStride` samples.
void CropSamples(std::span<const uint8> from, uint32 fromStride, uint32 toStride, uint32 texelBytes,
                 std::vector<uint8>& out)
{
    const uint32 inset = (fromStride - toStride) / 2u;
    const std::size_t rowBytes = static_cast<std::size_t>(toStride) * texelBytes;
    out.resize(rowBytes * toStride);
    for (uint32 r = 0; r < toStride; ++r)
        std::memcpy(out.data() + r * rowBytes,
                    from.data() + (static_cast<std::size_t>(r + inset) * fromStride + inset) * texelBytes, rowBytes);
}

enum class PageRemake : uint8
{
    Waiting,   // a page it reads is not written yet
    Unchanged, // the store holds these bytes
    Changed,
};

// Copies the owned samples of page (px, pz) of `level` that lie in `window`, decoded from `source`, a
// store whose pages have `sourceApron` samples of apron. False when the store does not hold the page.
bool GatherFinerPage(PageStoreWriter& source, const PageCodec& codec, uint32 level, uint32 px, uint32 pz,
                     uint32 sourceApron, FinerWindow& window)
{
    const PageAddress address{0, static_cast<uint8>(level), px, pz};
    const PageIndexEntry* entry = source.Layout().FindPresent(address);
    std::vector<uint8> stored;
    if (!entry || !source.ReadPage(address, stored))
        return false;
    const uint32 sourceStride = kPageOwnedSamples + 2u * sourceApron;
    std::vector<uint8> samples(static_cast<std::size_t>(sourceStride) * sourceStride * window.TexelBytes);
    codec.Decode(level, *entry, stored, samples);
    const uint32 x0 = std::max(px * kPageOwnedSamples, window.X.First);
    const uint32 x1 = std::min(px * kPageOwnedSamples + kPageOwnedSamples - 1u, window.X.Last);
    const uint32 z0 = std::max(pz * kPageOwnedSamples, window.Z.First);
    const uint32 z1 = std::min(pz * kPageOwnedSamples + kPageOwnedSamples - 1u, window.Z.Last);
    for (uint32 z = z0; z <= z1; ++z)
    {
        const std::size_t from = (static_cast<std::size_t>(z - pz * kPageOwnedSamples + sourceApron) * sourceStride +
                                  (x0 - px * kPageOwnedSamples + sourceApron)) *
                                 window.TexelBytes;
        std::memcpy(window.Texel(x0, z), samples.data() + from,
                    static_cast<std::size_t>(x1 - x0 + 1u) * window.TexelBytes);
    }
    return true;
}

// The stride samples of page (px, pz) of `level` of `layout`, filtered from the finer samples in
// `window`. False when the store does not hold the page or the window lacks a finer page it reads.
bool FilterCoarserPage(const PageStoreLayout& layout, uint32 level, uint32 px, uint32 pz, const FinerWindow& window,
                       std::vector<uint8>& outSamples)
{
    const uint32 stride = PageStrideTexels(layout.Header.Format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const PageStoreLevel& finer = layout.Levels[level - 1u];
    const PageStoreLevel& shape = layout.Levels[level];
    if (!layout.FindPresent(PageAddress{0, static_cast<uint8>(level), px, pz}) ||
        !window.Ready(OwningPages(FinerSamplesRead(px, apron, finer.SamplesX)),
                      OwningPages(FinerSamplesRead(pz, apron, finer.SamplesZ))))
        return false;
    outSamples.resize(static_cast<std::size_t>(stride) * stride * window.TexelBytes);
    for (uint32 r = 0; r < stride; ++r)
    {
        const int64 sampleZ = ClampIndex(static_cast<int64>(pz) * kPageOwnedSamples - apron + r, shape.SamplesZ);
        for (uint32 c = 0; c < stride; ++c)
        {
            const int64 sampleX = ClampIndex(static_cast<int64>(px) * kPageOwnedSamples - apron + c, shape.SamplesX);
            FilterTaps taps{};
            for (uint32 dz = 0; dz < 3u; ++dz)
                for (uint32 dx = 0; dx < 3u; ++dx)
                    taps[dz * 3u + dx] = window.Texel(ClampIndex(2 * sampleX - 1 + dx, finer.SamplesX),
                                                      ClampIndex(2 * sampleZ - 1 + dz, finer.SamplesZ));
            FilterCoarserTexel(layout.Header.Filter, taps, window.TexelBytes,
                               outSamples.data() + (static_cast<std::size_t>(r) * stride + c) * window.TexelBytes);
        }
    }
    return true;
}

} // namespace

PageStorePatcher::PageStorePatcher(PageStoreWriter& writer, JobSystem::WorkStealingThreadPool* pool)
    : m_Writer(writer)
    , m_Pool(pool)
    , m_Codec(MakePageCodec(writer.Layout().Header))
    , m_Written(writer.Layout().Levels.size())
{
}

PageStorePatcher::PageStorePatcher(PageStoreWriter& writer, PageStoreWriter& unencoded,
                                   JobSystem::WorkStealingThreadPool* pool)
    : m_Writer(writer)
    , m_Pool(pool)
    , m_Codec(MakePageCodec(writer.Layout().Header))
    , m_Unencoded(&unencoded)
    , m_UnencodedCodec(MakePageCodec(unencoded.Layout().Header))
    , m_Written(writer.Layout().Levels.size())
{
}

PageStorePatcher::~PageStorePatcher() = default;

bool PageStorePatcher::CanPatch() const
{
    const PageStoreHeader& header = m_Writer.Layout().Header;
    if (!m_Codec)
        return false;
    if (!m_Unencoded)
        return header.Format == PageFieldFormat::Rgba8 || header.Format == PageFieldFormat::Rgba8Srgb ||
               header.Format == PageFieldFormat::Rg8;
    const PageStoreHeader& samples = m_Unencoded->Layout().Header;
    return header.Format == PageFieldFormat::Bc5 && m_UnencodedCodec && samples.Format == PageFieldFormat::Rg8 &&
           samples.Filter == header.Filter && samples.SamplesX == header.SamplesX && samples.SamplesZ == header.SamplesZ;
}

void PageStorePatcher::EncodePage(uint32 level, std::span<const uint8> samples, EncodedPage& outEncoded,
                                  EncodedPage& outUnencoded) const
{
    m_Codec->Encode(level, samples, PageFootprint{}, outEncoded);
    if (!m_Unencoded)
        return;
    std::vector<uint8> cropped;
    CropSamples(samples, PageStrideTexels(PageFieldFormat::Bc5), PageStrideTexels(PageFieldFormat::Rg8),
                PageTexelBytes(PageFieldFormat::Rg8), cropped);
    m_UnencodedCodec->Encode(level, cropped, PageFootprint{}, outUnencoded);
}

bool PageStorePatcher::HoldsPage(const PageAddress& address, const EncodedPage& encoded,
                                 const EncodedPage& unencoded) const
{
    const PageIndexEntry* entry = m_Writer.Layout().FindPresent(address);
    if (!entry || entry->Offset == 0 || entry->Hash != encoded.Hash)
        return false;
    if (!m_Unencoded)
        return true;
    const PageIndexEntry* samples = m_Unencoded->Layout().FindPresent(address);
    return samples && samples->Offset != 0 && samples->Hash == unencoded.Hash;
}

bool PageStorePatcher::StorePage(const PageAddress& address, const EncodedPage& encoded, const EncodedPage& unencoded)
{
    return m_Writer.WritePage(address, encoded) && (!m_Unencoded || m_Unencoded->WritePage(address, unencoded));
}

bool PageStorePatcher::WritePage(const PageAddress& address, std::span<const uint8> samples)
{
    if (!CanPatch())
        return false;
    const PageFieldFormat format = m_Writer.Layout().Header.Format;
    const uint32 stride = PageStrideTexels(format);
    if (samples.size() != static_cast<std::size_t>(stride) * stride * PageTexelBytes(format))
        return false;
    if (!m_Writer.Layout().FindPresent(address) || (m_Unencoded && !m_Unencoded->Layout().FindPresent(address)))
        return false;
    EncodedPage encoded;
    EncodedPage unencoded;
    EncodePage(address.Level, samples, encoded, unencoded);
    if (HoldsPage(address, encoded, unencoded))
    {
        m_PagesUnchanged.fetch_add(1);
        return true;
    }
    if (!StorePage(address, encoded, unencoded))
        return false;
    m_PagesWritten.fetch_add(1);
    if (address.Level > 0u)
    {
        const PageSampleRect owned = OwnedSamples(address, m_Writer.Layout().Levels[address.Level]);
        std::lock_guard lock(m_WrittenMutex);
        std::optional<PageSampleRect>& written = m_Written[address.Level];
        written = written ? Union(*written, owned) : owned;
    }
    return true;
}

bool PageStorePatcher::RefilterAncestors(const PageSampleRect& level0Rect)
{
    if (!CanPatch())
        return false;
    const PageStoreLevel& level0 = m_Writer.Layout().Levels.front();
    Span changedX{std::min(level0Rect.MinX, level0.SamplesX - 1u), std::min(level0Rect.MaxX, level0.SamplesX - 1u)};
    Span changedZ{std::min(level0Rect.MinZ, level0.SamplesZ - 1u), std::min(level0Rect.MaxZ, level0.SamplesZ - 1u)};
    std::optional<PageSampleRect> changed = PageSampleRect{changedX.First, changedZ.First, changedX.Last, changedZ.Last};
    std::vector<std::optional<PageSampleRect>> written(m_Written.size());
    {
        std::lock_guard lock(m_WrittenMutex);
        written.swap(m_Written);
    }
    for (uint32 level = 1; level < m_Writer.Layout().Levels.size(); ++level)
    {
        const PageStoreLevel& shape = m_Writer.Layout().Levels[level];
        std::optional<PageSampleRect> coarser;
        if (changed)
        {
            const Span x = CoarserSamples({changed->MinX, changed->MaxX}, shape.SamplesX);
            const Span z = CoarserSamples({changed->MinZ, changed->MaxZ}, shape.SamplesZ);
            coarser = PageSampleRect{x.First, z.First, x.Last, z.Last};
        }
        if (!RefilterLevel(level, coarser, written[level], changed))
            return false;
    }
    return true;
}

bool PageStorePatcher::RefilterLevel(uint32 level, const std::optional<PageSampleRect>& changed,
                                     const std::optional<PageSampleRect>& written,
                                     std::optional<PageSampleRect>& outChanged)
{
    outChanged.reset();
    if (!changed && !written)
        return true;
    const PageStoreLayout& layout = m_Writer.Layout();
    const PageFieldFormat format = layout.Header.Format;
    const uint32 stride = PageStrideTexels(format);
    const uint32 apron = (stride - kPageOwnedSamples) / 2u;
    const uint32 texelBytes = PageTexelBytes(format);
    const PageStoreLevel& finer = layout.Levels[level - 1u];
    const PageStoreLevel& shape = layout.Levels[level];
    Span pagesX = written ? OwningPages({written->MinX, written->MaxX}) : Span{};
    Span pagesZ = written ? OwningPages({written->MinZ, written->MaxZ}) : Span{};
    if (changed)
    {
        const Span holdingX = PagesHolding({changed->MinX, changed->MaxX}, shape.PagesX, apron);
        const Span holdingZ = PagesHolding({changed->MinZ, changed->MaxZ}, shape.PagesZ, apron);
        pagesX = written ? Union(pagesX, holdingX) : holdingX;
        pagesZ = written ? Union(pagesZ, holdingZ) : holdingZ;
    }

    // The finer samples every remade page reads, from the pages that own them: a Bc5 store's from
    // its unencoded store.
    PageStoreWriter& source = m_Unencoded ? *m_Unencoded : m_Writer;
    const PageCodec& sourceCodec = m_Unencoded ? *m_UnencodedCodec : *m_Codec;
    const uint32 sourceStride = PageStrideTexels(source.Layout().Header.Format);
    const uint32 sourceApron = (sourceStride - kPageOwnedSamples) / 2u;
    FinerWindow window;
    window.TexelBytes = texelBytes;
    window.X = {FinerSamplesRead(pagesX.First, apron, finer.SamplesX).First,
                FinerSamplesRead(pagesX.Last, apron, finer.SamplesX).Last};
    window.Z = {FinerSamplesRead(pagesZ.First, apron, finer.SamplesZ).First,
                FinerSamplesRead(pagesZ.Last, apron, finer.SamplesZ).Last};
    window.PagesX = OwningPages(window.X);
    window.PagesZ = OwningPages(window.Z);
    window.Texels.resize(static_cast<std::size_t>(window.X.Count()) * window.Z.Count() * texelBytes);
    window.PageReady.assign(static_cast<std::size_t>(window.PagesX.Count()) * window.PagesZ.Count(), 0u);
    JobSystem::ParallelFor(m_Pool, window.PageReady.size(), [&](std::size_t i) {
        const uint32 px = window.PagesX.First + static_cast<uint32>(i % window.PagesX.Count());
        const uint32 pz = window.PagesZ.First + static_cast<uint32>(i / window.PagesX.Count());
        window.PageReady[i] = GatherFinerPage(source, sourceCodec, level - 1u, px, pz, sourceApron, window) ? 1u : 0u;
    });

    const std::size_t remakeCount = static_cast<std::size_t>(pagesX.Count()) * pagesZ.Count();
    std::vector<EncodedPage> encoded(remakeCount);
    std::vector<EncodedPage> unencoded(remakeCount);
    std::vector<PageRemake> remake(remakeCount, PageRemake::Waiting);
    JobSystem::ParallelFor(m_Pool, remakeCount, [&](std::size_t i) {
        const uint32 px = pagesX.First + static_cast<uint32>(i % pagesX.Count());
        const uint32 pz = pagesZ.First + static_cast<uint32>(i / pagesX.Count());
        std::vector<uint8> samples;
        if (!FilterCoarserPage(layout, level, px, pz, window, samples))
            return;
        EncodePage(level, samples, encoded[i], unencoded[i]);
        remake[i] = HoldsPage(PageAddress{0, static_cast<uint8>(level), px, pz}, encoded[i], unencoded[i])
                        ? PageRemake::Unchanged
                        : PageRemake::Changed;
    });

    // A page WritePage wrote is a new input to the level above, whatever this call made of it.
    if (written)
        outChanged = GrownSamples(*written, apron, shape);
    // Written in address order, so the store's bytes do not depend on the workers.
    for (std::size_t i = 0; i < remakeCount; ++i)
    {
        if (remake[i] == PageRemake::Unchanged)
            m_PagesUnchanged.fetch_add(1);
        if (remake[i] != PageRemake::Changed)
            continue;
        const PageAddress address{0, static_cast<uint8>(level), pagesX.First + static_cast<uint32>(i % pagesX.Count()),
                                  pagesZ.First + static_cast<uint32>(i / pagesX.Count())};
        if (!StorePage(address, encoded[i], unencoded[i]))
            return false;
        m_PagesWritten.fetch_add(1);
        const PageSampleRect stored = StoredSamples(address, apron, shape);
        outChanged = outChanged ? Union(*outChanged, stored) : stored;
    }
    return true;
}

} // namespace GameEngine::PageStreaming
