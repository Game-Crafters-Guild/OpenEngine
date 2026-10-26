#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{

struct EncodedPage;

/// Writes one page store, a page at a time in any order, then publishes it whole.
///
/// The store is assembled in a temporary sibling of its target and put in place by rename in
/// Finish, so a reader never opens a partial store. A writer destroyed before Finish removes its
/// temporary file. WritePage and ReadPage may be called from any number of threads at once;
/// Create, OpenForPatch, Finish and Abandon from one thread with no page call in flight.
class PageStoreWriter
{
public:
    PageStoreWriter() = default;
    ~PageStoreWriter();

    PageStoreWriter(const PageStoreWriter&) = delete;
    PageStoreWriter& operator=(const PageStoreWriter&) = delete;

    /// Starts a new store with `layout` (MakePageStoreLayout) for `target`. Every present page must
    /// be written before Finish; pages go into the file in the order they are written. Returns an empty string, else the reason.
    std::string Create(const std::filesystem::path& target, PageStoreLayout layout);

    /// Rewrites the store at `existing` (validated layout `layout`) as `target` under the key in `layout.Header`,
    /// by rename, not copy: a page not written keeps its bytes. Pages that `layout` adds (AddPresentPages) are
    /// written before Finish. Abandon discards the store. Returns an empty string, else the reason.
    std::string OpenForPatch(const std::filesystem::path& existing, const std::filesystem::path& target,
                             PageStoreLayout layout);

    const PageStoreLayout& Layout() const { return m_Layout; }

    /// Writes a present page's bytes and records its entry. A page keeps its place in the file while
    /// its size does; a first write, or one that changes the size, appends. False on an absent or
    /// out-of-store address, bytes that do not match the store's format and the page's flags, or an
    /// I/O failure.
    bool WritePage(const PageAddress& address, const EncodedPage& page);

    /// Reads back the stored bytes of a present page already written (or kept by a patch).
    bool ReadPage(const PageAddress& address, std::vector<uint8>& outBytes);

    /// Writes the header and index and publishes the store at its target. Returns an empty
    /// string, else the reason (the temporary file is removed).
    std::string Finish();

    /// Drops the store being written and removes its temporary file.
    void Abandon();

private:
    bool WriteAt(uint64 offset, const void* data, std::size_t bytes);
    bool WriteAtLocked(uint64 offset, const void* data, std::size_t bytes);
    bool ReadAt(uint64 offset, void* data, std::size_t bytes);

    std::filesystem::path m_Target;
    std::filesystem::path m_Temp;
    std::FILE* m_File = nullptr;
    std::mutex m_FileMutex; // one file position: every seek + transfer holds it
    PageStoreLayout m_Layout;
    uint64 m_End = 0; // the file's end: where the next appended page goes; under m_FileMutex
    bool m_Failed = false;
};

} // namespace GameEngine::PageStreaming
