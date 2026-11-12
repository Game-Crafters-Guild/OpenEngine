#pragma once

#include "FileWatcher/FileIdentity.h"
#include "Types/FunctionRef.h"
#include "Types/Types.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#ifdef PLATFORM_WINDOWS
struct _OVERLAPPED;
#endif

namespace GameEngine
{
#ifndef PLATFORM_WINDOWS
class DirectoryTreeWalk;
#endif

/**
 * @brief File change event types
 */
enum class FileChangeType
{
    Created,
    Modified,
    Deleted,
    Renamed
};

/**
 * @brief File change event data
 */
struct FileChangeEvent
{
    std::filesystem::path Path;
    std::filesystem::path OldPath; // For rename events
    FileChangeType Type;
    std::filesystem::file_time_type Timestamp;
};

/**
 * @brief Callback function for file change events
 */
using FileChangeCallback = Function<void(const FileChangeEvent&)>;

/**
 * @brief Cross-platform file system watcher.
 *
 * After the platform loses change events, the lost changes arrive later as ordinary
 * per-file events from a rescan, with no event of their own for the loss; on any
 * platform a directory created, renamed or deleted can arrive as per-file events
 * instead of one event for the directory, and a change can occasionally be heard twice.
 */
class FileWatcher
{
  public:
    FileWatcher();
    ~FileWatcher();

    /**
     * @brief Start watching a directory
     */
    bool StartWatching(const std::filesystem::path& directory, bool recursive = true);

    /**
     * @brief Stop watching and join the watch thread.
     * The change callback runs on that thread, so it must not call this or
     * wait on a thread that is calling this.
     */
    void StopWatching();

    /**
     * @brief Ask the watch thread to exit, without waiting for it.
     * Safe from any thread, including the change callback on the watch thread.
     * The thread still has to be joined by StopWatching from another thread.
     */
    void RequestStop();

    /**
     * @brief Check if currently watching
     */
    bool IsWatching() const
    {
        return m_WatchThread.joinable();
    }

    /**
     * @brief True from StartWatching until the watch thread returns.
     * Once it is false, StopWatching joins without waiting.
     */
    bool IsThreadRunning() const
    {
        return m_Watching.load();
    }

    /**
     * @brief Set callback for file change events.
     * Call only while not watching: the watch thread reads the callback with
     * no lock, so replacing it while that thread runs is a data race. To stop
     * deliveries, call StopWatching instead of clearing the callback.
     */
    void SetCallback(FileChangeCallback callback)
    {
        m_Callback = std::move(callback);
    }

    /**
     * @brief Add file extension filter (only watch these extensions)
     */
    void AddExtensionFilter(const String& extension);

    /**
     * @brief Clear extension filters (watch all files)
     */
    void ClearExtensionFilters();

    /**
     * @brief Set polling interval in milliseconds (for polling-based watching)
     */
    void SetPollingInterval(uint32 intervalMs)
    {
        m_PollingInterval = intervalMs;
    }

  private:
    /**
     * @brief Platform-specific watching implementation
     */
    void WatchingThread();

    /**
     * @brief Check the name rules and the excluded-directory rules for a path that the platform reported
     */
    bool ShouldWatchFile(const std::filesystem::path& path) const;

    /**
     * @brief Check if directory should be excluded from watching (obj/, bin/, etc.)
     */
    bool ShouldExcludeDirectory(const std::filesystem::path& path) const;

    /**
     * @brief Notify callback of file change
     */
    void NotifyFileChange(const FileChangeEvent& event);

#ifdef PLATFORM_WINDOWS
    /**
     * @brief Windows-specific watching using ReadDirectoryChangesW
     */
    void WatchWindows();

    /**
     * @brief Arm the next ReadDirectoryChangesW request on the watched directory.
     * @return ERROR_SUCCESS when a request is pending, otherwise the reason there is none.
     */
    uint32 IssueDirectoryRead(void* directory, _OVERLAPPED& overlapped, std::vector<uint8>& buffer);

    /**
     * @brief Process Windows file change notifications
     */
    void ProcessWindowsFileChanges(uint8* buffer, uint32 bufferSize);
#endif

    /**
     * @brief Polling-based watching for other platforms
     */
    void WatchPolling();

    /**
     * @brief Record every watched file in the snapshot the next scan diffs against.
     */
    void SnapshotWatchedFiles();

    /**
     * @brief Notify a change the platform reported itself, after recording it in the
     * snapshot, so a later rescan reports only the changes whose events were lost.
     */
    void NotifyReportedChange(const FileChangeEvent& event);

    /**
     * @brief Move the snapshot entries of the files under a renamed directory to its
     * new path.
     */
    void MoveSnapshotEntriesUnder(const std::filesystem::path& oldDirectory,
                                  const std::filesystem::path& newDirectory);

    /**
     * @brief Drop the snapshot entries of the files under a deleted directory.
     */
    void EraseSnapshotEntriesUnder(const std::filesystem::path& directory);

    /**
     * @brief React to the platform reporting that change events were lost (its
     * notification buffer overflowed): rescan the tree against the snapshot and report
     * every difference as an ordinary Created / Modified / Deleted / Renamed event, so
     * subscribers see the lost changes the way they see any other. A walk that
     * fails leaves the rescan owed to RetryOwedLostEventsRescan.
     */
    void RescanAfterLostEvents();

    /**
     * @brief Walk again for a lost-events rescan whose walk failed, if one is owed
     * and the last walk for it is at least one poll interval old. The Windows loop
     * calls it on each slice in which the platform reported nothing, until a walk
     * succeeds. The polling backend owes nothing: every poll walks the tree.
     */
    void RetryOwedLostEventsRescan();

    /**
     * @brief Check the name rules of a watched file: the engine's own files, build
     *        artifacts, hidden files and the extension filters.
     */
    bool IsWatchedFileName(std::string_view fileName) const;

    /**
     * @brief Visits every regular file under the watch directory that passes
     *        the extension and excluded-directory filters, with its last write time.
     * @return false when the watcher was asked to stop before the walk finished.
     */
    bool ForEachWatchedFile(
        FunctionRef<void(const std::filesystem::path&, std::filesystem::file_time_type)> visit);

    /**
     * @brief One polling scan: diffs the tree against the previous scan and
     *        reports Created / Modified / Deleted / Renamed.
     */
    void PollOnce();

    /**
     * @brief Compare one file that the scan found with the snapshot. Record a change
     *        for ReportScanChanges and change no snapshot entry.
     */
    void RecordScannedFile(const std::filesystem::path& path, std::filesystem::file_time_type lastWrite);

    /**
     * @brief Report the changes of a complete scan and apply them to the snapshot.
     */
    void ReportScanChanges();

    /**
     * @brief Sleep until the next poll is due, the watcher is stopping, or the
     * platform signals that something under the watch root changed.
     * Fires ProcessReadyDebouncedEvents at debounce granularity throughout.
     */
    void WaitForNextPoll();

#ifdef PLATFORM_MACOS
    /**
     * @brief Start/stop the FSEvents stream that wakes WaitForNextPoll.
     * It carries no event payload: the rescan is what classifies changes, so
     * the stream only has to say "look now". The poll interval stays as the
     * backstop for what FSEvents does not report (some network mounts).
     */
    bool StartChangeSignal();
    void StopChangeSignal();
#endif

    /**
     * @brief Debounced file change notification.
     * Modified events within the debounce window are deferred; other types fire immediately.
     */
    void NotifyFileChangeDebounced(const FileChangeEvent& event);

    /**
     * @brief Fire any pending debounced events whose deadline has elapsed.
     * Called from the watcher thread's loop so no separate debounce thread is needed.
     */
    void ProcessReadyDebouncedEvents();

  private:
    std::filesystem::path m_WatchDirectory;
    // Pre-computed normalized absolute watch root (set once in StartWatching, read-only after).
    std::filesystem::path m_NormalizedWatchBase;
    bool m_Recursive;
    // Set by StartWatching before the thread starts; the thread clears it as it returns.
    std::atomic<bool> m_Watching;
    std::atomic<bool> m_ShouldStop;
    std::thread m_WatchThread;
    FileChangeCallback m_Callback;
    Vector<String> m_ExtensionFilters;
    uint32 m_PollingInterval; // milliseconds
    mutable std::mutex m_Mutex;

    // Wake channel from a platform change signal to the polling loop's sleep.
    // The flag is the state; the condition variable only avoids a spin.
    std::mutex m_WakeMutex;
    std::condition_variable m_WakeCv;
    bool m_WakeRequested = false;
#ifdef PLATFORM_MACOS
    // FSEventStreamRef / dispatch_queue_t, held opaquely so this header stays
    // free of CoreServices and libdispatch.
    void* m_ChangeSignalStream = nullptr;
    void* m_ChangeSignalQueue = nullptr;
#endif

    // Snapshot of the watched tree: what the last scan, or the last event the
    // platform reported, saw for each watched file. The polling backend diffs
    // every scan against it. The Windows backend keeps it current from the
    // events it reports, an event for a directory moving or dropping the entries
    // of the files under it, and diffs a rescan against it when the platform
    // says events were lost. The identity pairs a path that vanished with one that
    // appeared in the same scan into a single Renamed event. It is read when a
    // path is first seen and again when its timestamp changes, because a
    // safe-save replaces the file behind the path and with it the identity.
    // Watcher thread only.
    struct PolledFile
    {
        std::filesystem::file_time_type LastWrite;
        FileIdentity Identity;
        // The number of the last scan that found the file.
        uint64 LastScan;
    };
    using PolledFileMap = FastHashMap<std::filesystem::path, PolledFile>;
    PolledFileMap m_PolledFiles;

    // The state of one scan, kept from scan to scan so that a scan of a tree
    // that did not change allocates nothing. Watcher thread only.
    struct ModifiedFile
    {
        // The snapshot entry. It stays valid until the snapshot gets or loses an entry.
        PolledFileMap::value_type* Entry;
        PolledFile Scanned;
    };
    struct AppearedFile
    {
        std::filesystem::path Path;
        PolledFile Scanned;
        bool IsRenameTarget;
    };
    uint64 m_ScanNumber = 0;
    std::vector<ModifiedFile> m_ScanModified;
    // Files that are not in the snapshot, in walk order. Their Created events wait
    // until the scan is complete: a file with the identity of a file that went in
    // the same scan is that file's new name, not a new file.
    std::vector<AppearedFile> m_ScanAppeared;
    FastHashMap<FileIdentity, size_t> m_ScanAppearedByIdentity;
#ifndef PLATFORM_WINDOWS
    std::unique_ptr<DirectoryTreeWalk> m_Walk;
    // The relative path that the excluded-directory rules read.
    String m_ExclusionScratch;
#endif
    // Whether the watch root was absent on the previous poll, so its going and
    // coming back are each logged once instead of every poll. A missing root is
    // a state (an asset mount mid-restage), not an error. Watcher thread only.
    bool m_WatchDirectoryMissing = false;
    // Whether a lost-events rescan's walk failed and none has succeeded since,
    // so the changes it was to report are still unreported. Watcher thread only.
    bool m_LostEventsRescanOwed = false;
    // When an owed rescan last walked. A walk of a large tree takes
    // hundreds of milliseconds, so an owed rescan walks at most once per poll
    // interval. Watcher thread only.
    std::chrono::steady_clock::time_point m_LastOwedRescanWalk;

    // Debouncing and duplicate prevention. All accessed from watcher thread only.
    HashMap<std::filesystem::path, std::chrono::steady_clock::time_point> m_LastEventTimes;
    static constexpr std::chrono::milliseconds DEBOUNCE_DELAY{100}; // 100ms debounce
    HashMap<std::filesystem::path, std::chrono::steady_clock::time_point> m_PendingEvents;

    // A replace-rename (safe-save: write a temp file, rename it over the
    // target) reports the clobbered target as Deleted immediately before the
    // Renamed that gives it new content — often in a separate notification
    // batch, so it can't be detected by lookahead. Deleted events are held
    // briefly so the follow-up event can cancel them; only a Deleted that
    // survives the hold is a real deletion.
    struct PendingDeletedEvent
    {
        FileChangeEvent Event;
        std::chrono::steady_clock::time_point Deadline;
    };
    static constexpr std::chrono::milliseconds kDeletedEventHold{200};
    HashMap<std::filesystem::path, PendingDeletedEvent> m_PendingDeletedEvents;

#ifdef PLATFORM_WINDOWS
    // Windows rename events arrive as OLD_NAME then NEW_NAME. We pair them so the
    // callback receives a single Renamed event with oldPath populated.
    std::filesystem::path m_PendingRenameOldPath;
    bool m_HasPendingRenameOldPath = false;
#endif

    DISALLOW_COPY_AND_ASSIGN(FileWatcher);

    friend class FileWatcherExclusionTest;
    friend class FileWatcherLostEventsTest;
};

} // namespace GameEngine
