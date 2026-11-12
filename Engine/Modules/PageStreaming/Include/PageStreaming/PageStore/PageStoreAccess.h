#pragma once

#include "PageStreaming/PageStore/PageStoreCommit.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "PageStreaming/PageSampleRect.h"
#include "PageStreaming/PageStore/PageStoreWriter.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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
    /// and Publish. A stream then reads again the absent pages that ReadableSince names, and the
    /// resident pages that a Commit names.
    uint64 Version() const { return m_Version.load(std::memory_order_acquire); }

    /// The changes after version `since`: appends each Commit to `outCommits` and returns the version
    /// they reach. Sets `outWhole` when any page can have become readable: a store was published
    /// (Publish, Finish), or the span is older than the changes the access keeps.
    uint64 ReadableSince(uint64 since, std::vector<PageStoreCommit>& outCommits, bool& outWhole) const;

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
    std::string AddPresentPages(std::span<const PageAddress> added);
    /// Tells the streams that the pages written since the last change are readable: the pages of
    /// `level` and coarser that read level-0 samples `level0Rect` (PageReadsLevel0Rect). A stream
    /// reads such a page again, absent or resident; it does not read a finer page again.
    void Commit(const PageSampleRect& level0Rect, uint32 level);
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
    // Records a change and moves Version to it: a Commit, or the whole store when empty.
    void AddChange(const std::optional<PageStoreCommit>& commit);

    // One change of Version: the pages a Commit made readable, or the whole store (a publication).
    struct ReadableChange
    {
        uint64 Version = 0;
        std::optional<PageStoreCommit> Commit;
    };
    // The changes a stream catches up with between two frames; older ones read as a whole change.
    static constexpr std::size_t kReadableChangesKept = 1024;

    PageFieldKind m_Kind;
    PageStoreHeader m_Shape;
    std::vector<PageStoreLevel> m_Levels;
    std::atomic<uint64> m_Version{0};
    mutable std::mutex m_ChangesMutex; // after m_Mutex when both are held
    std::deque<ReadableChange> m_Changes; // newest last, at most kReadableChangesKept
    mutable std::shared_mutex m_Mutex; // shared by reads, held alone by each layout change
    PageStoreWriter m_Writer;
    std::unique_ptr<PageCodec> m_WriterCodec; // the codec of the store being written
    bool m_Writing = false;
    std::filesystem::path m_File;              // the store's target while it is written
    std::unique_ptr<PageStoreReader> m_Reader; // the published store between runs, or null
};

} // namespace GameEngine::PageStreaming
