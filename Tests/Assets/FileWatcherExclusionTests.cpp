#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "FileWatcher/FileWatcher.h"
#include "Memory/AllocationCountScope.h"
#include "TestTempDir.h"

namespace GameEngine
{

class FileWatcherExclusionTest : public ::testing::Test
{
  protected:
    FileWatcher watcher;

    // ShouldExcludeDirectory absolutizes its argument before expressing it
    // relative to the watch base, so the base must be absolute ON THE HOST
    // PLATFORM. "C:/project" is absolute only on Windows; on POSIX it is a
    // relative path named "C:", lexically_relative() against it yields an
    // empty path, and the function's "cannot express relative to the root"
    // guard then answers "not excluded" for every input — which makes the
    // whole fixture silently inert rather than merely failing.
    static std::filesystem::path WatchRoot()
    {
#ifdef _WIN32
        return "C:/project";
#else
        return "/project";
#endif
    }

    void SetUp() override
    {
        // StartWatching() would normally populate these; set them directly so
        // the test needs no on-disk watch directory.
        watcher.m_WatchDirectory = WatchRoot();
        watcher.m_NormalizedWatchBase = WatchRoot().lexically_normal();
    }

    // Cases are written relative to the watch root so they read identically on
    // every platform and cannot re-introduce a host-specific absolute path.
    bool IsExcluded(const std::string& relativePath)
    {
        return watcher.ShouldExcludeDirectory(WatchRoot() / relativePath);
    }

    // Same call, but on a path passed through verbatim instead of joined onto the watch
    // root. Every private access must sit in this class: FileWatcher befriends the fixture,
    // and friendship is not inherited, so a TEST_F body — a derived class — cannot reach
    // ShouldExcludeDirectory itself.
    bool IsExcludedVerbatim(const std::string& absolutePath)
    {
        return watcher.ShouldExcludeDirectory(absolutePath);
    }
};

// --- Files inside excluded directories (trailing-slash patterns) ---

TEST_F(FileWatcherExclusionTest, ExcludesFileInsideGitDir)
{
    EXPECT_TRUE(IsExcluded(".git/index"));
    EXPECT_TRUE(IsExcluded(".git/refs/heads/main"));
}

TEST_F(FileWatcherExclusionTest, ExcludesGitDirItself)
{
    // Windows can report a change on the directory entry with no trailing slash.
    EXPECT_TRUE(IsExcluded(".git"));
}

TEST_F(FileWatcherExclusionTest, ExcludesFileInsideVsDir)
{
    EXPECT_TRUE(IsExcluded(".vs/settings.json"));
}

TEST_F(FileWatcherExclusionTest, ExcludesVsDirItself)
{
    EXPECT_TRUE(IsExcluded(".vs"));
}

TEST_F(FileWatcherExclusionTest, ExcludesBuildDir)
{
    EXPECT_TRUE(IsExcluded("build/CMakeCache.txt"));
    EXPECT_TRUE(IsExcluded("build"));
}

TEST_F(FileWatcherExclusionTest, ExcludesBinAndObjDirs)
{
    EXPECT_TRUE(IsExcluded("bin/Debug/app.exe"));
    EXPECT_TRUE(IsExcluded("obj/Release/temp.o"));
    EXPECT_TRUE(IsExcluded("bin"));
    EXPECT_TRUE(IsExcluded("obj"));
}

TEST_F(FileWatcherExclusionTest, ExcludesCacheDir)
{
    EXPECT_TRUE(IsExcluded(".Cache/shaders/foo.spv"));
    EXPECT_TRUE(IsExcluded(".Cache"));
}

// --- Paths that should NOT be excluded ---

TEST_F(FileWatcherExclusionTest, DoesNotExcludeNormalSourceFile)
{
    EXPECT_FALSE(IsExcluded("src/main.cpp"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeGitignoreAtRoot)
{
    // .gitignore is NOT inside .git/ — must not be excluded.
    EXPECT_FALSE(IsExcluded(".gitignore"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeGitattributes)
{
    EXPECT_FALSE(IsExcluded(".gitattributes"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeSimilarlyNamedDir)
{
    // A directory named "git-hooks" should not match the ".git" exclusion.
    EXPECT_FALSE(IsExcluded("git-hooks/pre-commit"));
}

TEST_F(FileWatcherExclusionTest, DoesNotExcludeAssetsDir)
{
    EXPECT_FALSE(IsExcluded("Assets/texture.png"));
}

// --- Windows backslash paths ---

TEST_F(FileWatcherExclusionTest, ExcludesWithBackslashSeparators)
{
#ifdef _WIN32
    // ReadDirectoryChangesW reports native separators. Passed whole rather
    // than through IsExcluded(), which would join them onto the watch root.
    EXPECT_TRUE(IsExcludedVerbatim("C:\\project\\.git\\index"));
    EXPECT_TRUE(IsExcludedVerbatim("C:\\project\\.git"));
#else
    // A backslash is a legal filename character on POSIX, not a separator, so
    // "a\\b" is one component and this scenario cannot arise.
    GTEST_SKIP() << "Backslash separators are Windows-specific.";
#endif
}

// --- Demo/Resources exception ---

TEST_F(FileWatcherExclusionTest, AllowsDemoResourcesEvenInExcludedParent)
{
    // The special exception for Demo/Resources should take precedence.
    EXPECT_FALSE(IsExcluded("build/Demo/Resources/test.txt"));
}

// --- Nested excluded dirs ---

TEST_F(FileWatcherExclusionTest, ExcludesNestedGitDir)
{
    // A .git directory nested inside a submodule path.
    EXPECT_TRUE(IsExcluded("submodule/.git/HEAD"));
    EXPECT_TRUE(IsExcluded("submodule/.git"));
}

// --- Stop latency ---

// StopWatching wakes the watch loop and joins it. It returns within one wait
// slice (50 ms on the Windows backend, immediately on the polling backends),
// never after a fixed sleep and never by abandoning a thread that still uses
// the watcher. Hot-reload rebinds stop and restart watchers on the main thread.
TEST(FileWatcherStop, StopReturnsWellInsideThePollInterval)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_fw_stop");
    ASSERT_TRUE(fs::create_directories(dir));

    constexpr uint32 kPollIntervalMs = 5000;
    constexpr auto kAllowed = std::chrono::milliseconds(100);

    FileWatcher watcher;
    watcher.SetPollingInterval(kPollIntervalMs);
    ASSERT_TRUE(watcher.StartWatching(dir, /*recursive=*/true));

    // Let the initial scan finish so the stop lands in the loop's wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto start = std::chrono::steady_clock::now();
    watcher.StopWatching();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::error_code ec;
    fs::remove_all(dir, ec);

    EXPECT_FALSE(watcher.IsWatching());
    EXPECT_LT(elapsed, kAllowed)
        << "StopWatching took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << " ms with a " << kPollIntervalMs << " ms poll interval";
}

// A walk of a large tree takes hundreds of milliseconds, and the first
// snapshot walks the whole tree. A stop that lands during the walk ends it
// instead of waiting for it.
TEST(FileWatcherStop, StopDuringTheFirstSnapshotReturnsWellInsideThePollInterval)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_fw_stop_walk");
    constexpr int kDirectories = 16;
    constexpr int kFilesPerDirectory = 500;
    for (int d = 0; d < kDirectories; ++d)
    {
        const fs::path sub = dir / ("dir_" + std::to_string(d));
        ASSERT_TRUE(fs::create_directories(sub));
        for (int f = 0; f < kFilesPerDirectory; ++f)
            std::ofstream(sub / ("file_" + std::to_string(f) + ".glsl")) << "walked";
    }

    constexpr uint32 kPollIntervalMs = 5000;
    constexpr auto kAllowed = std::chrono::milliseconds(100);
    constexpr auto kIntoTheWalk = std::chrono::milliseconds(20);

    FileWatcher watcher;
    watcher.SetPollingInterval(kPollIntervalMs);
    ASSERT_TRUE(watcher.StartWatching(dir, /*recursive=*/true));
    std::this_thread::sleep_for(kIntoTheWalk);

    const auto start = std::chrono::steady_clock::now();
    watcher.StopWatching();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::error_code ec;
    fs::remove_all(dir, ec);

    EXPECT_FALSE(watcher.IsWatching());
    EXPECT_LT(elapsed, kAllowed)
        << "StopWatching during the first snapshot of " << kDirectories * kFilesPerDirectory << " files took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms";
}

// --- Detection latency ---

#ifdef PLATFORM_MACOS
// The polling loop's sleep is interruptible: on macOS an FSEvents stream wakes
// it, so a write is noticed in FSEvents time rather than in poll time. Worth
// pinning because the poll interval is what a shader hot-reload's
// edit-to-pixels latency was mostly made of.
// Records what a watch thread reports, so a test can wait for one event.
class HeardEvents
{
  public:
    void OnChange(const FileChangeEvent& event)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Heard.emplace_back(event.Type, event.Path.filename().string());
        m_Changed.notify_all();
    }

    bool WaitFor(FileChangeType type, const std::string& name, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Changed.wait_for(lock, timeout, [&] {
            return std::find(m_Heard.begin(), m_Heard.end(), std::make_pair(type, name)) != m_Heard.end();
        });
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::vector<std::pair<FileChangeType, std::string>> m_Heard;
};

TEST(FileWatcherLatency, AnAddAChangeAndADeleteAreEachDetectedWellInsideThePollInterval)
{
    namespace fs = std::filesystem;
    using Milliseconds = std::chrono::milliseconds;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_fw_latency");
    ASSERT_TRUE(fs::create_directories(dir));
    const fs::path probe = dir / "probe.txt";

    // Long enough that a poll-only watcher cannot pass by accident: the
    // assertion allows a fifth of it.
    constexpr uint32 kPollIntervalMs = 5000;
    constexpr auto kAllowed = Milliseconds(kPollIntervalMs / 5);
    constexpr auto kTimeout = Milliseconds(kPollIntervalMs * 2);

    HeardEvents heard;
    FileWatcher watcher;
    watcher.SetPollingInterval(kPollIntervalMs);
    watcher.SetCallback([&heard](const FileChangeEvent& event) { heard.OnChange(event); });
    ASSERT_TRUE(watcher.StartWatching(dir, /*recursive=*/true));

    // Let the initial scan finish so the write below is a change rather than
    // part of the baseline snapshot.
    std::this_thread::sleep_for(Milliseconds(300));

    auto start = std::chrono::steady_clock::now();
    std::ofstream(probe) << "hello";
    const bool added = heard.WaitFor(FileChangeType::Created, "probe.txt", kTimeout);
    const auto addLatency = std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - start);

    // The timestamp moves a second on, so the scan sees the change whatever the timestamp resolution.
    start = std::chrono::steady_clock::now();
    const auto before = fs::last_write_time(probe);
    std::ofstream(probe, std::ios::trunc) << "changed";
    fs::last_write_time(probe, before + std::chrono::seconds(1));
    const bool changed = heard.WaitFor(FileChangeType::Modified, "probe.txt", kTimeout);
    const auto changeLatency = std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - start);

    start = std::chrono::steady_clock::now();
    fs::remove(probe);
    const bool deleted = heard.WaitFor(FileChangeType::Deleted, "probe.txt", kTimeout);
    const auto deleteLatency = std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - start);

    watcher.StopWatching();
    std::error_code ec;
    fs::remove_all(dir, ec);
    RecordProperty("addMs", static_cast<int>(addLatency.count()));
    RecordProperty("changeMs", static_cast<int>(changeLatency.count()));
    RecordProperty("deleteMs", static_cast<int>(deleteLatency.count()));

    // A Deleted is held for kDeletedEventHold before it fires, which the allowance covers.
    ASSERT_TRUE(added && changed && deleted)
        << "heard the add " << added << ", the change " << changed << ", the delete " << deleted;
    EXPECT_LT(addLatency, kAllowed) << "add detected in " << addLatency.count() << " ms";
    EXPECT_LT(changeLatency, kAllowed) << "change detected in " << changeLatency.count() << " ms";
    EXPECT_LT(deleteLatency, kAllowed) << "delete detected in " << deleteLatency.count() << " ms";
}
#endif // PLATFORM_MACOS

// --- Lost events ---

// A platform can lose change events: on Windows the notification buffer
// overflows and the request fails with ERROR_NOTIFY_ENUM_DIR or completes with
// no records. The watcher answers with a rescan of its tree that reports what
// changed as ordinary events. The fixture plays the backend's part on the
// calling thread (it reports some changes, loses others, then signals the
// loss), so the reaction is tested on every platform, without a watch thread.
class FileWatcherLostEventsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = TestUtils::MakeUniqueTempDirectory("ge_fw_lost");
        ASSERT_TRUE(std::filesystem::create_directories(m_Root / "sub"));
        // What StartWatching sets, without starting the watch thread.
        m_Watcher.m_WatchDirectory = m_Root;
        m_Watcher.m_NormalizedWatchBase = m_Root.lexically_normal();
        m_Watcher.m_Recursive = true;
        m_Watcher.SetCallback([this](const FileChangeEvent& event) { m_Events.push_back(event); });
    }

    void TearDown() override
    {
        std::error_code ec;
        // A test that stops early can leave a directory unreadable, which
        // remove_all cannot clear.
        std::filesystem::permissions(m_Root / "sub", std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(m_Root, ec);
    }

    void Write(const char* name, const char* text)
    {
        std::ofstream(m_Root / name, std::ios::binary | std::ios::trunc) << text;
    }

    // A rewrite moves the timestamp a second on, so the scan sees it whatever
    // the filesystem's timestamp resolution.
    void Rewrite(const char* name, const char* text)
    {
        const std::filesystem::path path = m_Root / name;
        const auto before = std::filesystem::last_write_time(path);
        Write(name, text);
        std::filesystem::last_write_time(path, before + std::chrono::seconds(1));
    }

    void Remove(const char* name) { std::filesystem::remove(m_Root / name); }

    void Rename(const char* from, const char* to) { std::filesystem::rename(m_Root / from, m_Root / to); }

    void SetReadable(const char* directory, bool readable)
    {
        std::filesystem::permissions(m_Root / directory, readable ? std::filesystem::perms::owner_all
                                                                  : std::filesystem::perms::none);
    }

    bool CanList(const char* directory) const
    {
        std::error_code ec;
        std::filesystem::directory_iterator listing(m_Root / directory, ec);
        return !ec;
    }

    void Snapshot() { m_Watcher.SnapshotWatchedFiles(); }

    // One tick of the polling backend, on the calling thread.
    void Poll() { m_Watcher.PollOnce(); }

    size_t SnapshotFileCount() const { return m_Watcher.m_PolledFiles.size(); }

    // A change the platform reported, handed on the way the Windows backend does.
    void Report(FileChangeType type, const char* name, const char* oldName = nullptr)
    {
        FileChangeEvent event;
        event.Path = m_Root / name;
        if (oldName != nullptr)
            event.OldPath = m_Root / oldName;
        event.Type = type;
        event.Timestamp = std::filesystem::file_time_type::clock::now();
        m_Watcher.NotifyReportedChange(event);
    }

    void LoseEvents() { m_Watcher.RescanAfterLostEvents(); }

    // What the Windows watch loop does on a slice in which the platform
    // reported nothing.
    void QuietSlice() { m_Watcher.RetryOwedLostEventsRescan(); }

    // A quiet slice one poll interval after the last walk for an owed rescan.
    void QuietSliceAPollIntervalLater()
    {
        m_Watcher.m_LastOwedRescanWalk -= std::chrono::milliseconds(m_Watcher.m_PollingInterval);
        m_Watcher.RetryOwedLostEventsRescan();
    }

    // The state a rescan whose walk has just failed leaves, without a walk that fails.
    void OweARescanThatJustWalked()
    {
        m_Watcher.m_LostEventsRescanOwed = true;
        m_Watcher.m_LastOwedRescanWalk = std::chrono::steady_clock::now();
    }

    // A Deleted is held before it fires, in case the path comes back. Waits out the hold, and a
    // margin past it so the event is due when the dispatch runs.
    void FireHeldEvents()
    {
        constexpr auto kPastTheHold = std::chrono::milliseconds(50);
        std::this_thread::sleep_for(FileWatcher::kDeletedEventHold + kPastTheHold);
        m_Watcher.ProcessReadyDebouncedEvents();
    }

    // What the subscriber heard, as sorted "Type path" lines relative to the root.
    std::vector<std::string> Heard() const
    {
        std::vector<std::string> lines;
        for (const FileChangeEvent& event : m_Events)
        {
            static const char* const kTypeNames[] = {"Created", "Modified", "Deleted", "Renamed"};
            std::string line = std::string(kTypeNames[static_cast<int>(event.Type)]) + " ";
            if (!event.OldPath.empty())
                line += event.OldPath.lexically_relative(m_Root).generic_string() + " -> ";
            line += event.Path.lexically_relative(m_Root).generic_string();
            lines.push_back(std::move(line));
        }
        std::sort(lines.begin(), lines.end());
        return lines;
    }

    std::filesystem::path m_Root;
    FileWatcher m_Watcher;
    std::vector<FileChangeEvent> m_Events;
};

TEST_F(FileWatcherLostEventsTest, LostChangesAreReportedAsOrdinaryEventsByTheRescan)
{
    Write("kept.glsl", "kept");
    Write("edited.glsl", "before");
    Write("removed.glsl", "removed");
    Write("old_name.glsl", "renamed");
    Write("sub/nested.glsl", "before");
    Snapshot();

    // Changes whose events the platform lost.
    Rewrite("edited.glsl", "after");
    Write("added.glsl", "added");
    Remove("removed.glsl");
    Rename("old_name.glsl", "new_name.glsl");
    Rewrite("sub/nested.glsl", "after");

    LoseEvents();
    FireHeldEvents();

    EXPECT_EQ(Heard(), (std::vector<std::string>{"Created added.glsl", "Deleted removed.glsl",
                                                 "Modified edited.glsl", "Modified sub/nested.glsl",
                                                 "Renamed old_name.glsl -> new_name.glsl"}));

    // The snapshot took each change, so the next rescan reports none of them again.
    m_Events.clear();
    LoseEvents();
    FireHeldEvents();
    EXPECT_EQ(Heard(), (std::vector<std::string>{}));
}

TEST_F(FileWatcherLostEventsTest, AChangeAlreadyReportedIsNotReportedAgainByTheRescan)
{
    Write("reported.glsl", "before");
    Write("reported_gone.glsl", "gone");
    Write("reported_old.glsl", "renamed");
    Write("lost.glsl", "before");
    Snapshot();

    // Changes the platform reported.
    Rewrite("reported.glsl", "after");
    Report(FileChangeType::Modified, "reported.glsl");
    Write("reported_new.glsl", "new");
    Report(FileChangeType::Created, "reported_new.glsl");
    Remove("reported_gone.glsl");
    Report(FileChangeType::Deleted, "reported_gone.glsl");
    Rename("reported_old.glsl", "reported_renamed.glsl");
    Report(FileChangeType::Renamed, "reported_renamed.glsl", "reported_old.glsl");
    FireHeldEvents();
    m_Events.clear();

    // One change whose event the platform lost.
    Rewrite("lost.glsl", "after");

    LoseEvents();
    FireHeldEvents();

    EXPECT_EQ(Heard(), (std::vector<std::string>{"Modified lost.glsl"}));
}

// A file can be gone again by the time its event is handled. Subscribers heard
// it exists, so if the event of its deletion is lost, the rescan reports it.
TEST_F(FileWatcherLostEventsTest, AReportedFileThatIsGoneAgainIsReportedDeletedByTheRescan)
{
    Snapshot();
    Report(FileChangeType::Created, "brief.glsl");
    m_Events.clear();

    LoseEvents();
    FireHeldEvents();

    EXPECT_EQ(Heard(), (std::vector<std::string>{"Deleted brief.glsl"}));
}

// The platform reports a directory renamed or deleted as one event for the
// directory. The files under it move or go with it, so a later rescan does not
// report them again, and still reports a lost change to one under its new name.
TEST_F(FileWatcherLostEventsTest, AReportedDirectoryRenameOrDeletionIsNotReportedAgainByTheRescan)
{
    std::filesystem::create_directories(m_Root / "sub" / "deep");
    std::filesystem::create_directories(m_Root / "doomed");
    std::filesystem::create_directories(m_Root / "subway");
    Write("sub/a.glsl", "a");
    Write("sub/b.glsl", "b");
    Write("sub/deep/c.glsl", "c");
    Write("doomed/d.glsl", "d");
    Write("subway/e.glsl", "e");
    Snapshot();

    // Changes the platform reported, one event per directory.
    Rename("sub", "moved");
    Report(FileChangeType::Renamed, "moved", "sub");
    std::filesystem::remove_all(m_Root / "doomed");
    Report(FileChangeType::Deleted, "doomed");
    FireHeldEvents();
    m_Events.clear();

    // One change whose event the platform lost.
    Rewrite("moved/a.glsl", "after");

    LoseEvents();
    FireHeldEvents();

    EXPECT_EQ(Heard(), (std::vector<std::string>{"Modified moved/a.glsl"}));
}

// A rescan's walk fails when a directory vanishes or cannot be read under it,
// which is likely while the burst that overflowed the platform's buffer is
// still landing. The rescan is then owed, and the watch loop walks again on a
// slice in which the platform reported nothing, once per poll interval, until a
// walk succeeds.
TEST_F(FileWatcherLostEventsTest, ARescanWhoseWalkFailsIsRetriedOnQuietSlicesUntilAWalkSucceeds)
{
    Write("kept.glsl", "before");
    Write("sub/nested.glsl", "before");
    Snapshot();

    // Changes whose events the platform lost.
    Write("added.glsl", "added");
    Rewrite("kept.glsl", "after");

    SetReadable("sub", false);
    if (CanList("sub"))
    {
        SetReadable("sub", true);
        GTEST_SKIP() << "permissions cannot make a directory unreadable here: Windows sets only the "
                        "read-only attribute, and a root user reads any directory";
    }
    LoseEvents();
    QuietSlice();
    FireHeldEvents();
    EXPECT_EQ(Heard(), (std::vector<std::string>{})) << "a walk that fails must report nothing";
    m_Events.clear();

    SetReadable("sub", true);
    QuietSliceAPollIntervalLater();
    FireHeldEvents();
    EXPECT_EQ(Heard(), (std::vector<std::string>{"Created added.glsl", "Modified kept.glsl"}));
    m_Events.clear();

    // Once a walk succeeds nothing is owed, so a quiet slice walks no more: a
    // change the platform has not reported stays unreported by the watcher.
    Rewrite("sub/nested.glsl", "after");
    QuietSlice();
    FireHeldEvents();
    EXPECT_EQ(Heard(), (std::vector<std::string>{}));
}

// A walk of a large tree takes hundreds of milliseconds. While a directory
// stays unreadable, an owed rescan walks again once per poll interval, not on
// every quiet slice.
TEST_F(FileWatcherLostEventsTest, AnOwedRescanWalksAtMostOncePerPollInterval)
{
    Write("kept.glsl", "kept");
    Snapshot();
    Write("added.glsl", "added");

    OweARescanThatJustWalked();
    QuietSlice();
    FireHeldEvents();
    EXPECT_EQ(Heard(), (std::vector<std::string>{})) << "an owed rescan walked again inside the poll interval";

    QuietSliceAPollIntervalLater();
    FireHeldEvents();
    EXPECT_EQ(Heard(), (std::vector<std::string>{"Created added.glsl"}));
}

// The polling backend walks every watched root once per poll interval while
// the editor is idle. A poll of a tree that did not change must not allocate
// after the first polls.
TEST_F(FileWatcherLostEventsTest, AWarmPollOfAnUnchangedTreeAllocatesNothing)
{
#ifdef _WIN32
    GTEST_SKIP() << "the Windows backend reports changes from the platform and walks only after lost events";
#else
    // Names longer than the small-string buffer, so a path copy cannot hide in it.
    const std::string longName = "a_file_name_longer_than_the_small_string_buffer_";
    const std::string deep = "sub/a_directory_with_a_long_name/another_directory_with_a_long_name/";
    std::filesystem::create_directories(m_Root / deep);
    std::filesystem::create_directories(m_Root / "build");
    constexpr int kFilesPerDirectory = 8;
    for (const std::string& directory : {std::string(), std::string("sub/"), deep})
    {
        for (int i = 0; i < kFilesPerDirectory; ++i)
            Write((directory + longName + std::to_string(i) + ".glsl").c_str(), "x");
    }
    // Files the filters skip: an excluded directory, a build extension in upper case, a dot file.
    Write("build/excluded_by_its_directory.glsl", "x");
    Write("sub/stale_safe_save_intermediate.TMP", "x");
    Write("sub/.hidden_dot_file", "x");
    // A link to a file is watched as that file. The walk does not follow a link to a directory.
    std::filesystem::create_symlink(m_Root / "sub" / (longName + "0.glsl"), m_Root / "link_to_a_file.glsl");
    std::filesystem::create_directory_symlink(m_Root / "sub", m_Root / "link_to_a_directory");

    Snapshot();
    Poll();
    Poll();
    ASSERT_EQ(SnapshotFileCount(), 3u * kFilesPerDirectory + 1);

    const Memory::AllocationCountScope probe(Memory::CountWindow::ThisThread);
    Poll();
    const uint64 allocations = probe.Count();

    EXPECT_EQ(allocations, 0u) << "a poll of " << SnapshotFileCount() << " unchanged files allocated "
                               << allocations << " times";
    EXPECT_EQ(Heard(), (std::vector<std::string>{}));
#endif
}

#ifdef _WIN32
// A subscriber whose first callback holds the watch thread until it is
// released, so the changes made meanwhile pile up in the platform's buffer.
class HoldingSubscriber
{
  public:
    void OnChange(const FileChangeEvent& event)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (event.Type == FileChangeType::Created || event.Type == FileChangeType::Modified)
            m_Heard.insert(event.Path.filename().string());
        m_Changed.notify_all();
        m_Changed.wait(lock, [this] { return m_Released; });
    }

    bool WaitUntilHeard(size_t count, std::chrono::seconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Changed.wait_for(lock, timeout, [this, count] { return m_Heard.size() >= count; });
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Released = true;
        m_Changed.notify_all();
    }

    size_t HeardCount()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Heard.size();
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::set<std::string> m_Heard;
    bool m_Released = false;
};

// The real platform path: a burst of files made while the watch thread is busy
// overflows the 64 KB notification buffer, Windows discards the buffered
// records, and the watcher's rescan reports every file of the burst.
TEST(FileWatcherWindowsLostEvents, ABurstThatOverflowsTheBufferIsReportedByTheRescan)
{
    namespace fs = std::filesystem;
    const fs::path dir = TestUtils::MakeUniqueTempDirectory("ge_fw_overflow");
    ASSERT_TRUE(fs::create_directories(dir));

    // Each record carries the file's name: this many long names fill the
    // buffer several times over.
    constexpr size_t kBurstFileCount = 2000;
    constexpr auto kHeardTimeout = std::chrono::seconds(30);
    const std::string longStem(100, 'x');

    HoldingSubscriber subscriber;
    FileWatcher watcher;
    watcher.SetCallback([&subscriber](const FileChangeEvent& event) { subscriber.OnChange(event); });
    ASSERT_TRUE(watcher.StartWatching(dir, /*recursive=*/true));
    // Let the initial snapshot finish, so the files below are changes.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::ofstream(dir / "first.txt") << "first";
    ASSERT_TRUE(subscriber.WaitUntilHeard(1, kHeardTimeout)) << "the watcher never reported the first file";
    for (size_t i = 0; i < kBurstFileCount; ++i)
        std::ofstream(dir / (longStem + std::to_string(i) + ".txt")) << "burst";
    subscriber.Release();

    const bool heardAll = subscriber.WaitUntilHeard(kBurstFileCount + 1, kHeardTimeout);
    const size_t heard = subscriber.HeardCount();
    watcher.StopWatching();
    std::error_code ec;
    fs::remove_all(dir, ec);
    EXPECT_TRUE(heardAll) << "heard " << heard << " of " << kBurstFileCount + 1 << " files";
}
#endif // _WIN32

} // namespace GameEngine
