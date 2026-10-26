#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStoreFormat.h"

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

/// Writes the pages of a byte-format or Bc5 store one at a time, and makes the coarser pages over a
/// changed rect again with the filter of PagePyramidBuilder: the result is the bytes of a full build.
/// WritePage runs on many threads at once; RefilterAncestors runs with no WritePage in flight.
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

    /// Remakes, level by level, each coarser page that reads `level0Rect` or a page written at the level below.
    /// A page whose inputs are not all written stays as it is until the call after its last input.
    /// False on a write error.
    bool RefilterAncestors(const PageSampleRect& level0Rect);

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
