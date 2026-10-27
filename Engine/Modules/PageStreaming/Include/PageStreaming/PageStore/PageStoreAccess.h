#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::PageStreaming
{

class PageCodec;
class PageStoreReader;
struct PageFetchOutcome;

/// One page store that streams read while a bake writes it: between runs through the published
/// store, during a run through the writer that patches it. Each layout change holds a lock alone
/// that reads share, so a read never gets moved bytes or a missing file. Fetch runs on workers.
class PageStoreAccess
{
public:
    /// A `kind` store on the lattice of `shape`: its samples, faces and format. A read finds no
    /// page until a store is published, made or opened.
    PageStoreAccess(PageFieldKind kind, const PageStoreHeader& shape);
    ~PageStoreAccess();

    PageStoreAccess(const PageStoreAccess&) = delete;
    PageStoreAccess& operator=(const PageStoreAccess&) = delete;

    /// The store's levels, faces and format. They do not change while the access lives.
    std::span<const PageStoreLevel> Levels() const { return m_Levels; }
    uint32 FaceCount() const { return m_Shape.FaceCount; }
    PageFieldFormat Format() const { return m_Shape.Format; }

    /// Changes each time a page that a read found absent can become readable: at Commit, Finish
    /// and Publish. A stream reads its absent pages again when it changes.
    uint64 Version() const { return m_Version.load(std::memory_order_acquire); }

    /// Reads and decodes page `address` (FetchPage). Absent when the store does not hold the
    /// page, has not written it yet, or no store is open.
    PageFetchOutcome Fetch(const PageAddress& address) const;

    /// Reads the published store at `file` from now on. Returns an empty string, else the reason
    /// (a store of another lattice, an unreadable file: no store is read then; a store is being
    /// written: nothing changes).
    std::string Publish(const std::filesystem::path& file);

    /// Starts a new store at `target` (PageStoreWriter::Create). Reads go to it from now on.
    std::string Create(const std::filesystem::path& target, PageStoreLayout layout);
    /// Opens the store at `file` to patch under `layout` (PageStoreWriter::OpenForPatch), to be
    /// published at `file` again by Finish. Reads go to the writer from now on.
    std::string OpenForPatch(const std::filesystem::path& file, PageStoreLayout layout);
    /// PageStoreWriter::AddPresentPages. The pages it adds read as absent until they are written.
    std::string AddPresentPages(std::span<const uint8> present);
    /// Tells the streams that the pages written since the last change are readable.
    void Commit();
    /// Publishes the store being written (PageStoreWriter::Finish) and reads it from now on.
    std::string Finish();
    /// Drops the store being written and lets the published store go: a read finds no page then.
    /// Call it before the store's file is deleted. Publish, Create or OpenForPatch reads a store again.
    void Close();

    /// The writer of the store being written: its page calls (WritePage, ReadPage) and its layout.
    /// Page calls only between the layout changes; the layout only on the thread that changes it.
    PageStoreWriter& Writer() { return m_Writer; }

private:
    bool MatchesShape(const PageStoreLayout& layout) const;
    std::string PublishLocked(const std::filesystem::path& file);

    PageFieldKind m_Kind;
    PageStoreHeader m_Shape;
    std::vector<PageStoreLevel> m_Levels;
    std::atomic<uint64> m_Version{0};
    mutable std::shared_mutex m_Mutex; // shared by reads, held alone by each layout change
    PageStoreWriter m_Writer;
    std::unique_ptr<PageCodec> m_WriterCodec; // the codec of the store being written
    bool m_Writing = false;
    std::filesystem::path m_File;              // the store's target while it is written
    std::unique_ptr<PageStoreReader> m_Reader; // the published store between runs, or null
};

} // namespace GameEngine::PageStreaming
