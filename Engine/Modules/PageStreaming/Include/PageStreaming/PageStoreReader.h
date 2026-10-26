#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class SharedFileReader;
}

namespace GameEngine::PageStreaming
{

class PageCodec;

/// Reads one page store: a .gepage file, or one field of a terrain container (.geterrain) at the
/// field's offset. The layout is read and validated once at Open; pages are read synchronously by
/// seek + read. Every page call may run on any thread (reads share one handle under a lock).
class PageStoreReader
{
public:
    PageStoreReader();
    ~PageStoreReader();

    PageStoreReader(const PageStoreReader&) = delete;
    PageStoreReader& operator=(const PageStoreReader&) = delete;

    /// Opens the `kind` store at `baseOffset` of `file`, occupying the rest of the file or
    /// `storeBytes` when non-zero. Returns an empty string, else the reason (the reader stays
    /// closed); a store whose format and filter are not `kind`'s is refused.
    std::string Open(const std::filesystem::path& file, PageFieldKind kind, uint64 baseOffset = 0,
                     uint64 storeBytes = 0);

    bool IsOpen() const { return m_IsOpen; }
    const PageStoreLayout& Layout() const { return m_Layout; }
    const std::filesystem::path& File() const { return m_File; }
    /// Where the store starts in File(); a page's file position is BaseOffset() + its entry's Offset.
    uint64 BaseOffset() const { return m_BaseOffset; }

    /// Reads a present page's stored bytes. False when the page is absent or the read fails.
    bool ReadPageBytes(const PageAddress& address, std::vector<uint8>& outBytes) const;

    /// Reads and decodes a present page into its samples with the store's codec: PageStrideTexels(format)^2
    /// samples of PageTexelBytes(format) bytes, row-major, apron included. False when the page is
    /// absent, `outSamples` is not the page's size, or the read fails.
    bool ReadPageSamples(const PageAddress& address, std::span<uint8> outSamples) const;

    /// A present page's loaded bytes from its stored bytes `stored`: PagePayloadBytes(format) bytes,
    /// the codec's samples, or a block format's blocks as stored. False when the page is absent or
    /// `stored` is not its stored size.
    bool DecodePayload(const PageAddress& address, std::span<const uint8> stored, std::vector<uint8>& outPayload) const;

private:
    std::filesystem::path m_File;
    uint64 m_BaseOffset = 0;
    PageStoreLayout m_Layout;
    bool m_IsOpen = false;
    std::unique_ptr<SharedFileReader> m_Reader;
    std::unique_ptr<PageCodec> m_Codec;
    mutable std::mutex m_ReadMutex;
};

} // namespace GameEngine::PageStreaming
