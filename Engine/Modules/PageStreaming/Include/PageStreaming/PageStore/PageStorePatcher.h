#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::PageStreaming
{

class PageCodec;
class PageStoreWriter;
struct EncodedPage;

/// Writes the pages of a byte-format or Bc5 store and refilters the coarser pages over a changed rect: a
/// coarser sample is the filter of the level below when every finer page its taps read is written, else
/// it keeps its stored value. WritePage runs on many threads; RefilterAncestors with no WritePage in flight.
class PageStorePatcher
{
public:
    /// Writes into `writer`. `pool` runs the filter and the encode; null runs them on this thread.
    PageStorePatcher(PageStoreWriter& writer, JobSystem::WorkStealingThreadPool* pool);
    /// Writes into the Bc5 store `writer` and into `unencoded`, an Rg8 store with the same lattice,
    /// filter and present pages. The coarser pages are filtered from `unencoded`, never from blocks.
    PageStorePatcher(PageStoreWriter& writer, PageStoreWriter& unencoded, JobSystem::WorkStealingThreadPool* pool);
    ~PageStorePatcher();

    PageStorePatcher(const PageStorePatcher&) = delete;
    PageStorePatcher& operator=(const PageStorePatcher&) = delete;

    /// Encodes one page's samples (PageStrideTexels(format)^2 texels of PageTexelBytes(format)
    /// bytes, row-major, apron included) and writes it at `address`. A page whose bytes the store
    /// already holds is not written. False on another format, a wrong sample count or a write error.
    bool WritePage(const PageAddress& address, std::span<const uint8> samples);

    /// Remakes, level by level, each coarser page that reads `level0Rect` (none: no level-0 change), a page
    /// written at its level or a page written at the level below. A sample whose taps read a finer page not yet
    /// written keeps its stored value; a page with such a sample that is not written yet stays unwritten.
    /// False on a write error.
    bool RefilterAncestors(const std::optional<PageSampleRect>& level0Rect);

    /// Pages written with new bytes, and pages whose bytes were already stored.
    uint64 PagesWritten() const { return m_PagesWritten.load(); }
    uint64 PagesUnchanged() const { return m_PagesUnchanged.load(); }

private:
    bool CanPatch() const;
    /// Writes one page's stride samples. False on a write error.
    bool StorePage(const PageAddress& address, const EncodedPage& encoded, const EncodedPage& unencoded);
    /// Encodes one page's stride samples for the store and, with an unencoded store, for it too.
    void EncodePage(uint32 level, std::span<const uint8> samples, EncodedPage& outEncoded,
                    EncodedPage& outUnencoded) const;
    /// The decoded stride samples the store holds for `address`; for a Bc5 store, the exact samples
    /// of its unencoded store where it has them. False when the page is not written.
    bool ReadStoredSamples(const PageAddress& address, std::vector<uint8>& outSamples) const;
    /// True when the store holds these bytes for `address`.
    bool HoldsPage(const PageAddress& address, const EncodedPage& encoded, const EncodedPage& unencoded) const;
    /// Remakes the pages of `level` that hold a sample of `changed`, a rect of that level, and the
    /// pages that own a sample of `written`. `outChanged` is the samples of the pages it rewrote.
    bool RefilterLevel(uint32 level, const std::optional<PageSampleRect>& changed,
                       const std::optional<PageSampleRect>& written, std::optional<PageSampleRect>& outChanged);

    PageStoreWriter& m_Writer;
    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    std::unique_ptr<PageCodec> m_Codec;
    PageStoreWriter* m_Unencoded = nullptr; // a Bc5 store's Rg8 samples; null for a byte format
    std::unique_ptr<PageCodec> m_UnencodedCodec;
    std::atomic<uint64> m_PagesWritten{0};
    std::atomic<uint64> m_PagesUnchanged{0};
    std::mutex m_WrittenMutex;
    // One a level: the owned samples of the pages WritePage wrote since the last RefilterAncestors.
    std::vector<std::optional<PageSampleRect>> m_Written;
};

} // namespace GameEngine::PageStreaming
