#include "FileWatcher/FileWatcher.h"
#include "Logger/Logger.h"
#include "Platform/Thread.h"
#include "Types/PathUtils.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <string_view>
#include <vector>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include "DirectoryTreeWalk.h"
#endif

#ifdef PLATFORM_MACOS
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace GameEngine
{

namespace
{
// Longest single wait in either watch loop. Debounced events fire at this
// granularity, and m_ShouldStop is rechecked at least this often.
constexpr uint32 kWatchLoopSliceMs = 50;

// Build artifacts that can appear next to watched assets: the editor's own .pdb
// and .ilk, and the .tmp siblings of atomic saves. Compared without case, because
// ReplaceFile-style saves (Photoshop, Office) write "name~RFxxxx.TMP".
constexpr std::string_view kBuildExtensions[] = {
    ".tmp", ".pdb", ".ilk", ".exp", ".obj", ".lib", ".exe", ".dll",
};

// Directories of build artifacts and caches, matched anywhere in the path under
// the watch root. A change in them does not trigger a hot reload.
constexpr std::string_view kExcludedDirectories[] = {
    "/obj/",           // .NET build intermediate files
    "/bin/",           // .NET build output files
    "/.vs/",           // Visual Studio files
    "/.vscode/",       // VS Code files
    "/node_modules/",  // Node.js dependencies
    "/.git/",          // Git repository files
    "/.Cache/",        // Engine/Editor derived caches (AssetDbCache.sqlite, shader cache, etc.)
    "/.Editor/",       // Editor derived cache (thumbnails, layout, etc.)
    "/__pycache__/",   // Python bytecode cache (common inside scripts/venv)
    "/site-packages/", // Python venv packages (can be huge; never engine assets)
    "/.venv/",         // Python virtualenv (common naming)
    "/venv/",          // Python virtualenv (common naming)
    "/__MACOSX/",      // macOS archive metadata folder
    "/build/",         // CMake build directory (relative to watch dir)
    "/Debug/",         // Debug build artifacts
    "/Release/"        // Release build artifacts
};

// The extension of a file name, as std::filesystem::path::extension finds it:
// from the last '.', and none when that '.' starts the name.
std::string_view FileNameExtension(std::string_view fileName)
{
    if (fileName == "." || fileName == "..")
        return {};
    const size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    return fileName.substr(dot);
}

// Whether a path under the watch root is in an excluded directory, or is one.
// relative uses '/' between names. scratch keeps its capacity between calls.
bool IsExcludedRelativePath(std::string_view relative, String& scratch)
{
    // A path that is not under the root is not excluded here. Higher layers can filter it.
    if (relative.empty() || relative == "." || relative.starts_with(".."))
        return false;

    // A leading '/' lets a pattern like "/obj/" match a directory at the root.
    scratch.assign(relative);
    std::replace(scratch.begin(), scratch.end(), '\\', '/');
    if (scratch.front() != '/')
        scratch.insert(scratch.begin(), '/');

    // Demo resources stay watched for hot reload, also in an excluded parent.
    if (scratch.find("/Demo/Resources") != String::npos)
        return false;

    for (const std::string_view excluded : kExcludedDirectories)
    {
        if (scratch.find(excluded) != String::npos)
            return true;

        // Also match the directory itself (no trailing slash): Windows can report
        // a change on the directory entry, e.g. "/submodule/.git" for "/.git/".
        if (scratch.ends_with(excluded.substr(0, excluded.size() - 1)))
            return true;
    }
    return false;
}

#ifndef PLATFORM_WINDOWS
// Closes the walk's directories when a walk stops early or throws.
class CloseWalkOnExit
{
  public:
    explicit CloseWalkOnExit(DirectoryTreeWalk& walk) : m_Walk(walk) {}
    ~CloseWalkOnExit() { m_Walk.Close(); }
    CloseWalkOnExit(const CloseWalkOnExit&) = delete;
    CloseWalkOnExit& operator=(const CloseWalkOnExit&) = delete;

  private:
    DirectoryTreeWalk& m_Walk;
};
#endif
} // namespace

#ifdef PLATFORM_MACOS
namespace
{
// FSEvents coalescing window. Small enough that an editor save reaches the
// rescan in well under a frame's worth of human latency, large enough that a
// multi-file save wakes the loop once rather than per file.
constexpr CFAbsoluteTime kChangeSignalLatencySeconds = 0.05;
} // namespace

bool FileWatcher::StartChangeSignal()
{
    CFStringRef path = CFStringCreateWithCString(nullptr, m_WatchDirectory.c_str(),
                                                 kCFStringEncodingUTF8);
    if (path == nullptr)
        return false;
    CFArrayRef paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1,
                                     &kCFTypeArrayCallBacks);
    CFRelease(path);
    if (paths == nullptr)
        return false;

    FSEventStreamContext context{};
    context.info = this;
    // The callback is a wake, not a source of events: it deliberately ignores
    // the reported paths and flags, because the rescan below it is what decides
    // what actually changed (and applies the extension/exclusion filters). That
    // covers a report of dropped events too (kFSEventStreamEventFlagMustScanSubDirs,
    // UserDropped, KernelDropped): it arrives through this callback like any
    // other, and the rescan it wakes walks the whole tree.
    auto onEvent = [](ConstFSEventStreamRef, void* info, size_t, void*,
                      const FSEventStreamEventFlags*, const FSEventStreamEventId*)
    {
        auto* self = static_cast<FileWatcher*>(info);
        {
            std::lock_guard<std::mutex> lock(self->m_WakeMutex);
            self->m_WakeRequested = true;
        }
        self->m_WakeCv.notify_one();
    };

    FSEventStreamRef stream = FSEventStreamCreate(
        nullptr, onEvent, &context, paths, kFSEventStreamEventIdSinceNow,
        kChangeSignalLatencySeconds,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (stream == nullptr)
        return false;

    dispatch_queue_t queue =
        dispatch_queue_create("com.gameengine.filewatcher.fsevents", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream))
    {
        FSEventStreamSetDispatchQueue(stream, nullptr);
        FSEventStreamRelease(stream);
        dispatch_release(queue);
        return false;
    }

    m_ChangeSignalStream = stream;
    m_ChangeSignalQueue = queue;
    return true;
}

void FileWatcher::StopChangeSignal()
{
    if (m_ChangeSignalStream != nullptr)
    {
        auto stream = static_cast<FSEventStreamRef>(m_ChangeSignalStream);
        FSEventStreamStop(stream);
        // Stop + unschedule + invalidate prevent NEW callback invocations from
        // being enqueued, but FSEvents does not document that any of them waits
        // for a callback that is already executing — the drain below is what
        // guarantees that.
        FSEventStreamSetDispatchQueue(stream, nullptr);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        m_ChangeSignalStream = nullptr;
    }
    if (m_ChangeSignalQueue != nullptr)
    {
        auto queue = static_cast<dispatch_queue_t>(m_ChangeSignalQueue);
        // Serial queue: an empty synchronous block cannot start until a callback
        // already on the queue has returned. After this line no callback can be
        // touching `this` (it captures the watcher), so releasing the queue and
        // tearing the watcher down is safe.
        dispatch_sync_f(queue, nullptr, [](void*) {});
        dispatch_release(queue);
        m_ChangeSignalQueue = nullptr;
    }
}
#endif // PLATFORM_MACOS


FileWatcher::FileWatcher()
    : m_Recursive(false), m_Watching(false), m_ShouldStop(false), m_PollingInterval(1000) // 1 second default
{
#ifndef PLATFORM_WINDOWS
    m_Walk = std::make_unique<DirectoryTreeWalk>();
#endif
}

FileWatcher::~FileWatcher()
{
    StopWatching();
}

bool FileWatcher::StartWatching(const std::filesystem::path& directory, bool recursive)
{
    if (m_WatchThread.joinable())
    {
        Logger::Log::Warning("FileWatcher already has a running thread");
        return true;
    }

    if (!std::filesystem::exists(directory))
    {
        Logger::Log::Error("Directory does not exist: {}", directory.string());
        return false;
    }

    m_WatchDirectory = directory;
    m_Recursive = recursive;
    m_ShouldStop = false;

    // Pre-compute normalized absolute base path once (read-only after this point).
    std::error_code ec;
    m_NormalizedWatchBase = std::filesystem::absolute(m_WatchDirectory, ec);
    if (ec)
        m_NormalizedWatchBase = m_WatchDirectory;
    m_NormalizedWatchBase = m_NormalizedWatchBase.lexically_normal();

    // Start watching thread
    m_Watching = true;
    m_WatchThread = std::thread(&FileWatcher::WatchingThread, this);

    Logger::Log::Debug("Started watching directory: {} (recursive: {})",
                       directory.string(), recursive);
    return true;
}

void FileWatcher::StopWatching()
{
    if (!m_WatchThread.joinable())
    {
        m_PendingEvents.clear();
        m_LostEventsRescanOwed = false;
#ifdef PLATFORM_WINDOWS
        m_PendingRenameOldPath.clear();
        m_HasPendingRenameOldPath = false;
#endif
        return;
    }

    Logger::Log::Debug("Stopping file watcher");

    // join() has no timeout because no wait in the watch loop outlasts
    // kWatchLoopSliceMs: the polling loop waits on m_WakeCv, which RequestStop
    // ends at once, and the Windows loop rechecks m_ShouldStop after each
    // WaitForSingleObject slice, and a scan ends at its next entry. Stop returns
    // within one slice plus any callback already running. A new blocking call in
    // either loop must keep that bound (a timed slice, or a wake from
    // RequestStop), or stop hangs.
    RequestStop();
    m_WatchThread.join();

    m_Watching = false;

    // The watch thread has exited, so its pending state is safe to clear.
    m_PendingEvents.clear();
    m_LostEventsRescanOwed = false;
#ifdef PLATFORM_WINDOWS
    m_PendingRenameOldPath.clear();
    m_HasPendingRenameOldPath = false;
#endif

    Logger::Log::Debug("File watcher stopped");
}

void FileWatcher::RequestStop()
{
    m_ShouldStop = true;
    {
        std::lock_guard<std::mutex> lock(m_WakeMutex);
        m_WakeRequested = true;
    }
    m_WakeCv.notify_all();
}

void FileWatcher::AddExtensionFilter(const String& extension)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ExtensionFilters.push_back(extension);
}

void FileWatcher::ClearExtensionFilters()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ExtensionFilters.clear();
}

void FileWatcher::WatchingThread()
{
    Platform::SetCurrentThreadName("File Watcher");
    Logger::Log::Debug("FileWatcher thread started for directory: {}", m_WatchDirectory.string());

#ifdef PLATFORM_WINDOWS
    WatchWindows();
#else
#ifdef PLATFORM_MACOS
    // Owned by this thread so the stream's lifetime is strictly inside the
    // loop that its callback wakes.
    const bool signalled = StartChangeSignal();
    Logger::Log::Info("FileWatcher: polling every {} ms, FSEvents wake {}", m_PollingInterval,
                      signalled ? "active" : "unavailable (poll interval is the only latency)");
#endif
    WatchPolling();
#ifdef PLATFORM_MACOS
    StopChangeSignal();
#endif
#endif

    m_Watching = false;
}

bool FileWatcher::ShouldWatchFile(const std::filesystem::path& path) const
{
    return IsWatchedFileName(path.filename().string()) && !ShouldExcludeDirectory(path);
}

bool FileWatcher::IsWatchedFileName(std::string_view fileName) const
{
    // The engine's own persistence files: watching them makes a feedback loop.
    if (fileName == "AssetDatabase.assetdb" || fileName == "AssetDatabase.assetdb.tmp")
        return false;

    // Hidden files (.DS_Store, .gitkeep), as AssetRegistry ignores them.
    if (!fileName.empty() && fileName.front() == '.')
        return false;

    // A build-artifact intermediate that passes takes the identity of the asset it replaces.
    const std::string_view extension = FileNameExtension(fileName);
    for (const std::string_view buildExtension : kBuildExtensions)
    {
        if (EqualsIgnoreCase(extension, buildExtension))
            return false;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    // With no filters, every file is watched.
    if (m_ExtensionFilters.empty())
        return true;
    for (const String& filter : m_ExtensionFilters)
    {
        if (EqualsIgnoreCase(extension, filter))
            return true;
    }
    return false;
}

bool FileWatcher::ShouldExcludeDirectory(const std::filesystem::path& path) const
{
    // A lexical path under the watch root. std::filesystem::relative can throw for a path that is gone,
    // and an absolute path can match "/build/" above the root.
    // This allocates; the POSIX walk does not call it.
    const auto& base = m_NormalizedWatchBase;
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec)
        abs = path;
    abs = abs.lexically_normal();

    String scratch;
    return IsExcludedRelativePath(abs.lexically_relative(base).generic_string(), scratch);
}

void FileWatcher::NotifyFileChange(const FileChangeEvent& event)
{
    if (m_Callback && ShouldWatchFile(event.Path))
    {
        // RACE CONDITION FIX: Use debounced notification to prevent multiple triggers
        NotifyFileChangeDebounced(event);
    }
}

#ifdef PLATFORM_WINDOWS
uint32 FileWatcher::IssueDirectoryRead(void* directory, OVERLAPPED& overlapped, std::vector<uint8>& buffer)
{
    // Reset overlapped state (except for the event handle) before re-issuing.
    HANDLE evt = overlapped.hEvent;
    overlapped = {};
    overlapped.hEvent = evt;
    ResetEvent(overlapped.hEvent);

    // Start async directory change monitoring. When using OVERLAPPED, lpBytesReturned is ignored.
    BOOL result = ReadDirectoryChangesW(
        directory,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        m_Recursive ? TRUE : FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE,
        nullptr,
        &overlapped,
        nullptr);

    if (result)
    {
        return ERROR_SUCCESS;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        return ERROR_SUCCESS; // Expected for overlapped I/O
    }
    if (error == ERROR_OPERATION_ABORTED || m_ShouldStop.load())
    {
        Logger::Log::Debug("ReadDirectoryChangesW operation aborted (shutdown)");
        return error;
    }
    if (error != ERROR_NOTIFY_ENUM_DIR)
    {
        Logger::Log::Error("ReadDirectoryChangesW failed: {}", error);
    }
    return error;
}

void FileWatcher::WatchWindows()
{
    // Open directory handle
    HANDLE hDir = CreateFileW(
        m_WatchDirectory.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        Logger::Log::Error("❌ Failed to open directory for watching: {} (error: {})", m_WatchDirectory.string(), error);
        return;
    }

    // Buffer for change notifications
    constexpr DWORD bufferSize = 64 * 1024; // 64KB buffer
    std::vector<uint8> buffer(bufferSize);
    OVERLAPPED overlapped = {};
    // Manual-reset event simplifies re-issuing overlapped operations safely.
    overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    if (!overlapped.hEvent)
    {
        Logger::Log::Error("Failed to create event for file watching: {}", GetLastError());
        CloseHandle(hDir);
        return;
    }

    Logger::Log::Debug("FileWatcher: ReadDirectoryChangesW armed for {}", m_WatchDirectory.string());

    // Issue the first async read; then only re-issue after we have processed a completion.
    if (IssueDirectoryRead(hDir, overlapped, buffer) != ERROR_SUCCESS)
    {
        // Cleanup below
        CancelIoEx(hDir, &overlapped);
        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        Logger::Log::Debug("Windows file watching stopped");
        return;
    }

    // The snapshot a lost-events rescan diffs against. Taken after the first
    // request is armed, so a change made during the walk is either in the
    // snapshot or reported by an event.
    try
    {
        SnapshotWatchedFiles();
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        // A rescan then reports the files this walk missed as created.
        Logger::Log::Error("FileWatcher: initial scan of '{}' failed: {}", m_WatchDirectory.string(), e.what());
    }

    bool hasPendingRead = true;
    while (!m_ShouldStop.load())
    {
        // Wait one slice so debounced events fire on time and m_ShouldStop is
        // rechecked; nothing signals this event on stop.
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kWatchLoopSliceMs);

        // Fire any debounced events whose deadline has elapsed
        ProcessReadyDebouncedEvents();

        if (waitResult == WAIT_TIMEOUT)
        {
            // A quiet slice: the burst a failed rescan walk ran into has likely
            // landed, so an owed rescan walks again, at most once per poll interval.
            RetryOwedLostEventsRescan();
            continue; // keep waiting on the same pending read
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED || m_ShouldStop.load())
            {
                Logger::Log::Debug("WaitForSingleObject operation aborted (shutdown)");
                break;
            }
            Logger::Log::Error("WaitForSingleObject failed: {}", error);
            break;
        }

        // The platform reports lost events in two shapes: the request fails with
        // ERROR_NOTIFY_ENUM_DIR, or it completes with no records because its
        // buffer overflowed and was discarded. Either way the changes it held are
        // reported by a rescan.
        DWORD bytesReturned = 0;
        if (!GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED || m_ShouldStop.load())
            {
                Logger::Log::Debug("GetOverlappedResult operation aborted (shutdown)");
                break;
            }
            if (error == ERROR_NOTIFY_ENUM_DIR)
            {
                RescanAfterLostEvents();
            }
            else
            {
                Logger::Log::Error("GetOverlappedResult failed: {}", error);
            }
        }
        else if (bytesReturned == 0)
        {
            RescanAfterLostEvents();
        }
        else
        {
            ProcessWindowsFileChanges(buffer.data(), bytesReturned);
        }

        hasPendingRead = false;

        if (m_ShouldStop.load())
        {
            break;
        }

        // Re-issue for the next set of changes. A request refused because events
        // were already lost is answered the same way, then issued again.
        DWORD issueError = IssueDirectoryRead(hDir, overlapped, buffer);
        while (issueError == ERROR_NOTIFY_ENUM_DIR && !m_ShouldStop.load())
        {
            RescanAfterLostEvents();
            issueError = IssueDirectoryRead(hDir, overlapped, buffer);
        }
        if (issueError != ERROR_SUCCESS)
        {
            break;
        }
        hasPendingRead = true;
    }

    // Cancel any pending overlapped operations before cleanup
    if (hasPendingRead)
    {
        CancelIoEx(hDir, &overlapped);
    }

    // Cleanup
    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
    Logger::Log::Debug("Windows file watching stopped");
}

void FileWatcher::ProcessWindowsFileChanges(uint8* buffer, uint32 bufferSize)
{
    if (!buffer || bufferSize == 0)
    {
        return;
    }

    const uint8* const begin = buffer;
    const uint8* const end = buffer + bufferSize;
    const uint8* ptr = begin;

    constexpr size_t kHeaderSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);

    while (ptr + kHeaderSize <= end)
    {
        const FILE_NOTIFY_INFORMATION* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

        const uint32 fileNameBytes = info->FileNameLength;
        if ((fileNameBytes % sizeof(wchar_t)) != 0)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (odd FileNameLength={}): ignoring remaining data", fileNameBytes);
            break;
        }

        const size_t recordSize = kHeaderSize + static_cast<size_t>(fileNameBytes);
        if (ptr + recordSize > end)
        {
            Logger::Log::Warning("Truncated FILE_NOTIFY_INFORMATION (recordSize={} > remaining={}): ignoring remaining data",
                                 recordSize, static_cast<size_t>(end - ptr));
            break;
        }

        // Convert filename from wide string to regular string (not null-terminated)
        std::wstring wfilename(info->FileName, fileNameBytes / sizeof(wchar_t));
        std::filesystem::path filePath = m_WatchDirectory / std::filesystem::path(wfilename);

        // Skip if this is in an excluded directory OR the specific file is filtered
        // (build artifacts, persistence .tmp files, hidden files, etc.). ShouldWatchFile
        // catches file-level exclusions that ShouldExcludeDirectory doesn't (e.g.
        // AssetDatabase.assetdb.tmp in the watch root itself).
        if (ShouldExcludeDirectory(filePath) || !ShouldWatchFile(filePath))
        {
            // Avoid log spam: excluded paths (build output, .tmp files, etc.) can
            // change very frequently and are not interesting to the asset system.
        }
        else
        {
            auto emit = [this](FileChangeEvent& event)
            {
                Logger::Log::Debug("Windows file change detected: {} ({})",
                                   event.Path.string(), static_cast<int>(event.Type));
                NotifyReportedChange(event);
            };

            const auto nowTs = std::filesystem::file_time_type::clock::now();

            switch (info->Action)
            {
            case FILE_ACTION_ADDED:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Created;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            case FILE_ACTION_REMOVED:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Deleted;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            case FILE_ACTION_MODIFIED:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Modified;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            case FILE_ACTION_RENAMED_OLD_NAME:
            {
                // Store for pairing with FILE_ACTION_RENAMED_NEW_NAME.
                // If an old name is already pending (unexpected), flush it as a delete
                // so the system does not get stuck with a stale pending path.
                if (m_HasPendingRenameOldPath && !m_PendingRenameOldPath.empty())
                {
                    FileChangeEvent flush;
                    flush.Path = m_PendingRenameOldPath;
                    flush.Type = FileChangeType::Deleted;
                    flush.Timestamp = nowTs;
                    emit(flush);
                }
                m_PendingRenameOldPath = filePath;
                m_HasPendingRenameOldPath = true;
                break;
            }
            case FILE_ACTION_RENAMED_NEW_NAME:
            {
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Renamed;
                event.Timestamp = nowTs;
                if (m_HasPendingRenameOldPath)
                {
                    event.OldPath = m_PendingRenameOldPath;
                    m_PendingRenameOldPath.clear();
                    m_HasPendingRenameOldPath = false;
                }
                emit(event);
                break;
            }
            default:
            {
                // Treat unknown actions as modified.
                FileChangeEvent event;
                event.Path = filePath;
                event.Type = FileChangeType::Modified;
                event.Timestamp = nowTs;
                emit(event);
                break;
            }
            }
        }

        // Move to next notification
        const uint32 next = info->NextEntryOffset;
        if (next == 0)
        {
            break;
        }

        // Validate next offset stays within the returned buffer and makes forward progress.
        if ((next % 4) != 0)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (NextEntryOffset={} not 4-byte aligned): ignoring remaining data", next);
            break;
        }
        if (next < recordSize)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (NextEntryOffset={} < recordSize={}): ignoring remaining data", next, recordSize);
            break;
        }
        if (ptr + next > end)
        {
            Logger::Log::Warning("Malformed FILE_NOTIFY_INFORMATION (NextEntryOffset={} exceeds remaining={}): ignoring remaining data",
                                 next, static_cast<size_t>(end - ptr));
            break;
        }

        ptr += next;
    }
}
#endif

#ifdef PLATFORM_WINDOWS
bool FileWatcher::ForEachWatchedFile(
    FunctionRef<void(const std::filesystem::path&, std::filesystem::file_time_type)> visit)
{
    // A stop is checked at every entry: a walk of a large tree takes hundreds of
    // milliseconds, and StopWatching waits for the walk.
    if (m_Recursive)
    {
        std::filesystem::recursive_directory_iterator iter(m_WatchDirectory);
        const std::filesystem::recursive_directory_iterator end;
        for (; iter != end; ++iter)
        {
            if (m_ShouldStop.load(std::memory_order_relaxed))
                return false;
            const auto& entry = *iter;
            if (entry.is_directory() && ShouldExcludeDirectory(entry.path()))
            {
                iter.disable_recursion_pending();
                continue;
            }
            if (entry.is_regular_file() && ShouldWatchFile(entry.path()))
                visit(entry.path(), entry.last_write_time());
        }
        return true;
    }

    for (const auto& entry : std::filesystem::directory_iterator(m_WatchDirectory))
    {
        if (m_ShouldStop.load(std::memory_order_relaxed))
            return false;
        if (entry.is_regular_file() && ShouldWatchFile(entry.path()))
            visit(entry.path(), entry.last_write_time());
    }
    return true;
}
#else
bool FileWatcher::ForEachWatchedFile(
    FunctionRef<void(const std::filesystem::path&, std::filesystem::file_time_type)> visit)
{
    // A stop is checked at every entry: a walk of a large tree takes hundreds of
    // milliseconds, and StopWatching waits for the walk.
    DirectoryTreeWalk& walk = *m_Walk;
    const CloseWalkOnExit closeWalk(walk);
    walk.Begin(m_WatchDirectory);
    while (walk.Next())
    {
        if (m_ShouldStop.load(std::memory_order_relaxed))
            return false;

        const DirectoryTreeWalk::EntryType type = walk.CurrentType();
        if (type == DirectoryTreeWalk::EntryType::Directory)
        {
            if (m_Recursive && !IsExcludedRelativePath(walk.CurrentRelativePath(), m_ExclusionScratch))
                walk.Descend();
            continue;
        }

        std::error_code error;
        if (type == DirectoryTreeWalk::EntryType::Symlink)
        {
            // A link to a file is watched as that file. The walk does not follow a link to a directory.
            const std::filesystem::file_status target = std::filesystem::status(walk.CurrentPath(), error);
            if (error && target.type() != std::filesystem::file_type::not_found)
                throw std::filesystem::filesystem_error("status", walk.CurrentPath(), error);
            if (!std::filesystem::is_regular_file(target))
                continue;
        }
        else if (type != DirectoryTreeWalk::EntryType::RegularFile)
        {
            continue;
        }

        if (!IsWatchedFileName(walk.CurrentName()) ||
            IsExcludedRelativePath(walk.CurrentRelativePath(), m_ExclusionScratch))
            continue;
        const std::filesystem::file_time_type lastWrite = std::filesystem::last_write_time(walk.CurrentPath(), error);
        if (error)
            throw std::filesystem::filesystem_error("last_write_time", walk.CurrentPath(), error);
        visit(walk.CurrentPath(), lastWrite);
    }
    return true;
}
#endif

void FileWatcher::SnapshotWatchedFiles()
{
    ForEachWatchedFile([this](const std::filesystem::path& path, std::filesystem::file_time_type lastWrite) {
        m_PolledFiles[path] = PolledFile{lastWrite, ReadFileIdentity(path), 0};
    });
}

void FileWatcher::NotifyReportedChange(const FileChangeEvent& event)
{
    // The snapshot holds what subscribers have been told, recorded before they
    // are told: a write that lands after the record moves the timestamp, so a
    // later rescan still reports it. A path with no entry of its own is, in the
    // common case, a directory: the platform reports a directory renamed or
    // deleted as one event, and the files under it move or go with it.
    if (!event.OldPath.empty() && m_PolledFiles.erase(event.OldPath) == 0)
        MoveSnapshotEntriesUnder(event.OldPath, event.Path);
    if (event.Type == FileChangeType::Deleted)
    {
        if (m_PolledFiles.erase(event.Path) == 0)
            EraseSnapshotEntriesUnder(event.Path);
    }
    else
    {
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::status(event.Path, ec);
        if (!std::filesystem::exists(status))
        {
            // Gone again already. Subscribers are about to hear the path exists,
            // so it keeps an entry with no timestamp, and a rescan that finds it
            // missing reports the deletion.
            m_PolledFiles[event.Path] = PolledFile{std::filesystem::file_time_type::min(), FileIdentity{}, 0};
        }
        else if (std::filesystem::is_regular_file(status))
        {
            const auto lastWrite = std::filesystem::last_write_time(event.Path, ec);
            m_PolledFiles[event.Path] =
                PolledFile{ec ? std::filesystem::file_time_type::min() : lastWrite, ReadFileIdentity(event.Path), 0};
        }
        // A directory gets no entry: a scan sees files only. So the files in a
        // directory that appears are reported by a rescan as created.
    }
    NotifyFileChange(event);
}

void FileWatcher::MoveSnapshotEntriesUnder(const std::filesystem::path& oldDirectory,
                                           const std::filesystem::path& newDirectory)
{
    std::vector<std::pair<std::filesystem::path, PolledFile>> moved;
    for (auto it = m_PolledFiles.begin(); it != m_PolledFiles.end();)
    {
        const std::filesystem::path relative = RelativePathUnderRoot(it->first, oldDirectory);
        if (relative.empty())
        {
            ++it;
            continue;
        }
        // Renaming a directory keeps its files' timestamps and identities.
        moved.emplace_back(newDirectory / relative, it->second);
        it = m_PolledFiles.erase(it);
    }
    for (auto& [path, polled] : moved)
        m_PolledFiles[std::move(path)] = polled;
}

void FileWatcher::EraseSnapshotEntriesUnder(const std::filesystem::path& directory)
{
    erase_if(m_PolledFiles, [&directory](const auto& entry)
             { return !RelativePathUnderRoot(entry.first, directory).empty(); });
}

void FileWatcher::RescanAfterLostEvents()
{
    Logger::Log::Warning("FileWatcher: change events under '{}' were lost; rescanning to report what changed",
                         m_WatchDirectory.string());
#ifdef PLATFORM_WINDOWS
    // An old name still waiting for its new one may have lost it. The rescan
    // reports that rename, or the deletion, itself.
    m_PendingRenameOldPath.clear();
    m_HasPendingRenameOldPath = false;
#endif
    try
    {
        PollOnce();
        m_LostEventsRescanOwed = false;
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        // A walk fails when a directory vanishes or cannot be read under it,
        // which is likely while the burst of changes that overflowed the
        // platform's buffer is still landing. A failed walk reports nothing, so
        // walking again reports every change once.
        m_LostEventsRescanOwed = true;
        m_LastOwedRescanWalk = std::chrono::steady_clock::now();
        Logger::Log::Warning("FileWatcher: rescan of '{}' failed ({}); while the watcher is idle it walks again "
                             "once per poll interval, until a walk succeeds",
                             m_WatchDirectory.string(), e.what());
    }
}

void FileWatcher::RetryOwedLostEventsRescan()
{
    if (!m_LostEventsRescanOwed)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_LastOwedRescanWalk < std::chrono::milliseconds(m_PollingInterval))
        return;
    m_LastOwedRescanWalk = now;
    try
    {
        PollOnce();
    }
    catch (const std::filesystem::filesystem_error&)
    {
        // Still owed. The failure was logged when the rescan fell due; a walk
        // that keeps failing would repeat it every poll interval.
        return;
    }
    m_LostEventsRescanOwed = false;
    Logger::Log::Info("FileWatcher: the rescan of '{}' owed since events were lost has reported what changed",
                      m_WatchDirectory.string());
}

void FileWatcher::WatchPolling()
{
    Logger::Log::Debug("Using polling-based file watching");

    try
    {
        SnapshotWatchedFiles();
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        Logger::Log::Error("Error during initial file scan: {}", e.what());
        return;
    }

    while (!m_ShouldStop.load())
    {
        std::error_code ec;
        const bool present = std::filesystem::exists(m_WatchDirectory, ec) && !ec;
        if (!present)
        {
            // A missing root is a state, not an error: an asset mount can be
            // deleted and re-staged as a unit. Pause the poll so the walk does
            // not throw every tick, and keep m_PolledFiles so the tree is diffed
            // against it when the root returns rather than replayed as new.
            if (!m_WatchDirectoryMissing)
            {
                m_WatchDirectoryMissing = true;
                Logger::Log::Warning("FileWatcher: '{}' is gone; watching pauses until it returns",
                                     m_WatchDirectory.string());
            }
        }
        else
        {
            if (m_WatchDirectoryMissing)
            {
                m_WatchDirectoryMissing = false;
                Logger::Log::Info("FileWatcher: '{}' is back; watching resumes", m_WatchDirectory.string());
            }
            try
            {
                PollOnce();
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                // The root can vanish between the check above and the walk;
                // report only a genuine error, not that narrow race.
                if (std::filesystem::exists(m_WatchDirectory))
                    Logger::Log::Error("Error during file watching: {}", e.what());
            }
        }

        WaitForNextPoll();
    }
}

void FileWatcher::WaitForNextPoll()
{
    // Wait in debounce-sized slices so pending debounced events still fire on
    // time, and cut the wait short as soon as a platform change signal arrives.
    // The poll interval stays the upper bound, so a platform with no signal —
    // or one that drops an event — behaves exactly as it did before.
    uint32 remainingMs = m_PollingInterval;
    while (remainingMs > 0 && !m_ShouldStop.load())
    {
        const uint32 sliceMs = std::min(remainingMs, kWatchLoopSliceMs);
        bool woken = false;
        {
            std::unique_lock<std::mutex> lock(m_WakeMutex);
            woken = m_WakeCv.wait_for(lock, std::chrono::milliseconds(sliceMs),
                                      [this] { return m_WakeRequested; });
            m_WakeRequested = false;
        }
        ProcessReadyDebouncedEvents();
        if (woken)
            return;
        remainingMs -= sliceMs;
    }
}

void FileWatcher::PollOnce()
{
    // Nothing is reported until the walk completes: a walk that throws or stops
    // reports nothing and leaves the snapshot as it was, so the next walk reports
    // each change once.
    ++m_ScanNumber;
    m_ScanModified.clear();
    m_ScanAppeared.clear();
    m_ScanAppearedByIdentity.clear();
    const bool walked = ForEachWatchedFile(
        [this](const std::filesystem::path& path, std::filesystem::file_time_type lastWrite)
        { RecordScannedFile(path, lastWrite); });
    if (!walked)
        return;
    ReportScanChanges();
}

void FileWatcher::RecordScannedFile(const std::filesystem::path& path, std::filesystem::file_time_type lastWrite)
{
    const auto previous = m_PolledFiles.find(path);
    if (previous == m_PolledFiles.end())
    {
        const PolledFile scanned{lastWrite, ReadFileIdentity(path), m_ScanNumber};
        // The first file with an identity wins: hard links share one.
        if (scanned.Identity.Valid)
            m_ScanAppearedByIdentity.emplace(scanned.Identity, m_ScanAppeared.size());
        m_ScanAppeared.push_back(AppearedFile{path, scanned, false});
        return;
    }

    previous->second.LastScan = m_ScanNumber;
    if (previous->second.LastWrite == lastWrite)
        return;
    m_ScanModified.push_back(ModifiedFile{&*previous, PolledFile{lastWrite, ReadFileIdentity(path), m_ScanNumber}});
}

void FileWatcher::ReportScanChanges()
{
    for (const ModifiedFile& modified : m_ScanModified)
    {
        modified.Entry->second = modified.Scanned;
        FileChangeEvent event;
        event.Path = modified.Entry->first;
        event.Type = FileChangeType::Modified;
        event.Timestamp = modified.Scanned.LastWrite;
        NotifyFileChange(event);
    }

    // Every path the previous scan saw that is gone now. Its identity showing
    // up under a new path in this scan, with the timestamp it had, is a
    // rename; anything else was deleted. The timestamp is part of the match
    // because an inode is not unique over time: ext4 and xfs hand a deleted
    // file's inode to the next file created, so within one poll "delete A,
    // create B" can present B under A's identity. rename() preserves mtime
    // and a new file has a new one, so a mismatch falls through to
    // Deleted + Created; a rename followed by an edit inside one poll takes
    // that path too, which is what every poll reported before pairing.
    bool anyGone = false;
    for (const auto& [path, polled] : m_PolledFiles)
    {
        if (polled.LastScan == m_ScanNumber)
            continue;
        anyGone = true;

        FileChangeEvent event;
        auto renamedTo = polled.Identity.Valid ? m_ScanAppearedByIdentity.find(polled.Identity)
                                               : m_ScanAppearedByIdentity.end();
        if (renamedTo != m_ScanAppearedByIdentity.end() &&
            m_ScanAppeared[renamedTo->second].Scanned.LastWrite != polled.LastWrite)
        {
            renamedTo = m_ScanAppearedByIdentity.end();
        }
        if (renamedTo != m_ScanAppearedByIdentity.end())
        {
            AppearedFile& renamed = m_ScanAppeared[renamedTo->second];
            renamed.IsRenameTarget = true;
            event.Path = renamed.Path;
            event.OldPath = path;
            event.Type = FileChangeType::Renamed;
            event.Timestamp = renamed.Scanned.LastWrite;
            m_ScanAppearedByIdentity.erase(renamedTo);
            Logger::Log::Debug("FileWatcher: '{}' -> '{}' paired as a rename by file identity",
                               event.OldPath.string(), event.Path.string());
        }
        else
        {
            event.Path = path;
            event.Type = FileChangeType::Deleted;
            event.Timestamp = std::filesystem::file_time_type::clock::now();
        }
        NotifyFileChange(event);
    }
    if (anyGone)
        erase_if(m_PolledFiles, [this](const auto& entry) { return entry.second.LastScan != m_ScanNumber; });

    for (AppearedFile& appeared : m_ScanAppeared)
    {
        if (!appeared.IsRenameTarget)
        {
            FileChangeEvent event;
            event.Path = appeared.Path;
            event.Type = FileChangeType::Created;
            event.Timestamp = appeared.Scanned.LastWrite;
            NotifyFileChange(event);
        }
        m_PolledFiles.insert_or_assign(std::move(appeared.Path), appeared.Scanned);
    }
}

void FileWatcher::NotifyFileChangeDebounced(const FileChangeEvent& event)
{
    if (!m_Callback)
        return;

    const auto now = std::chrono::steady_clock::now();

    // Hold Deleted events instead of firing them: a replace-rename (or a
    // delete-then-rewrite save) makes the path reappear within the hold
    // window, and tearing down the asset's registration in between would lose
    // its identity. ProcessReadyDebouncedEvents fires the survivors.
    if (event.Type == FileChangeType::Deleted)
    {
        m_PendingEvents.erase(event.Path);
        m_LastEventTimes[event.Path] = now;
        m_PendingDeletedEvents[event.Path] = {event, now + kDeletedEventHold};
        return;
    }

    // Preserve semantics for Created/Renamed and ensure oldPath isn't lost:
    // only debounce Modified events.
    if (event.Type != FileChangeType::Modified)
    {
        // The path reappeared — this was a replacement, not a deletion.
        if (m_PendingDeletedEvents.erase(event.Path) > 0)
        {
            Logger::Log::Debug("FileWatcher: '{}' reappeared before its Deleted fired "
                               "(replace-rename); suppressing the delete",
                               event.Path.string());
        }

        // Drop any pending debounced Modified event for this path (and oldPath for rename)
        m_PendingEvents.erase(event.Path);
        if (!event.OldPath.empty())
        {
            m_PendingEvents.erase(event.OldPath);
        }

        m_LastEventTimes[event.Path] = now;
        if (!event.OldPath.empty())
        {
            m_LastEventTimes[event.OldPath] = now;
        }

        m_Callback(event);
        return;
    }

    // A Modified event also proves the path exists again (Created records can
    // be coalesced away) — cancel any held Deleted for it.
    if (m_PendingDeletedEvents.erase(event.Path) > 0)
    {
        Logger::Log::Debug("FileWatcher: '{}' reappeared before its Deleted fired "
                           "(replace-rename); suppressing the delete",
                           event.Path.string());
    }

    // Debounce Modified events to prevent multiple triggers.
    bool shouldSchedule = false;
    bool shouldFireNow = false;
    auto& lastEventTime = m_LastEventTimes[event.Path];
    if (now - lastEventTime < DEBOUNCE_DELAY)
    {
        shouldSchedule = true;
    }
    else
    {
        lastEventTime = now;
        shouldFireNow = true;
    }

    if (shouldSchedule)
    {
        Logger::Log::Debug("Debouncing file change event for: {} (too soon)", event.Path.string());
        m_PendingEvents[event.Path] = now + DEBOUNCE_DELAY;
        return;
    }

    if (shouldFireNow)
    {
        Logger::Log::Debug("File change event passed debounce check: {}", event.Path.string());
        m_Callback(event);
    }
}

void FileWatcher::ProcessReadyDebouncedEvents()
{
    if (m_PendingEvents.empty() && m_PendingDeletedEvents.empty())
        return;

    auto now = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> readyEvents;

    for (auto it = m_PendingEvents.begin(); it != m_PendingEvents.end();)
    {
        if (now >= it->second)
        {
            readyEvents.push_back(it->first);
            it = m_PendingEvents.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& path : readyEvents)
    {
        if (m_Callback && ShouldWatchFile(path))
        {
            FileChangeEvent event;
            event.Path = path;
            event.Type = FileChangeType::Modified;
            event.Timestamp = std::filesystem::file_time_type::clock::now();

            Logger::Log::Debug("Firing debounced file change event: {}", path.string());

            m_LastEventTimes[path] = now;
            m_Callback(event);
        }
    }

    // Held Deleted events that survived the hold window are real deletions.
    std::vector<FileChangeEvent> readyDeleted;
    for (auto it = m_PendingDeletedEvents.begin(); it != m_PendingDeletedEvents.end();)
    {
        if (now >= it->second.Deadline)
        {
            readyDeleted.push_back(it->second.Event);
            it = m_PendingDeletedEvents.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& event : readyDeleted)
    {
        if (m_Callback && ShouldWatchFile(event.Path))
        {
            Logger::Log::Debug("Firing held Deleted event: {}", event.Path.string());
            m_LastEventTimes[event.Path] = now;
            m_Callback(event);
        }
    }
}

} // namespace GameEngine
