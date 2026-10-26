// The shader compile key is built from a source's #include closure: every
// included file is hashed, and scanned for its own directives, before the
// compile cache can be probed. One engine header reaches dozens of programs, so
// each file is read and resolved once and then held in memory.
//
// Holding content is only safe while an edit cannot survive it. The cache is
// ANNOUNCED rather than validated: it never re-reads a held file, and an edit is
// seen once NotifyShaderSourcesChanged runs (the editor's watcher lane and every
// in-process writer call it). The tests below prove both halves: an unannounced
// change is not read -- removing the file from disk is how they show the bytes
// came from memory -- and an announced one always is, including the cases a
// timestamp check cannot see and the ones that move where a directive resolves.
#include <gtest/gtest.h>

#include "Rendering/Core/HotReloadIntegration.h"
#include "Source/Materials/ShaderIncludeClosure.h"
#include "Source/Materials/ShaderSourceCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

void WriteFile(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

uint64_t HashOf(const std::string& text)
{
    return HashShaderBytes(text.data(), text.size());
}

// Renames `from` over `to`, the way editors save. A reader holding the target
// open can refuse the rename on Windows: retry until it has let go.
void RenameOver(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    do
    {
        fs::rename(from, to, ec);
        if (ec)
            std::this_thread::yield();
    } while (ec);
}

// How long a test lets a reader thread run before it changes the file the
// reader is reading, so the change lands while the read is in flight.
constexpr auto kReaderHeadStart = std::chrono::microseconds(300);

// The text of `version` of the header shared_<header>.glsl.
std::string SharedHeaderVersion(int header, int version)
{
    return "// version " + std::to_string(version) + " of shared_" + std::to_string(header) + ".glsl\n";
}

// Concurrent closure walks of one shader, and what they saw.
struct ConcurrentWalks
{
    const std::string& Shader;
    fs::path Root;
    // Every hash each header ever had on disk, by header.
    const std::vector<std::vector<uint64_t>>& ValidHashes;
    std::atomic<bool> Done{false};
    std::atomic<int> Torn{0};
    std::atomic<int> Walks{0};
};

// Walks the shader's closure until `walks.Done`, counting each walk and each
// header it folded that was never on disk, or lost.
void WalkUntilDone(ConcurrentWalks& walks)
{
    const size_t headers = walks.ValidHashes.size();
    while (!walks.Done.load(std::memory_order_acquire))
    {
        IncludeClosure closure;
        CollectIncludeClosure(walks.Shader, walks.Root / "walker.glsl", {walks.Root}, closure);
        walks.Walks.fetch_add(1, std::memory_order_relaxed);
        if (closure.hashes.size() != headers)
        {
            walks.Torn.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (size_t h = 0; h < headers; ++h)
        {
            const auto& valid = walks.ValidHashes[h];
            if (std::find(valid.begin(), valid.end(), closure.hashes[h]) == valid.end())
                walks.Torn.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

class ShaderSourceCacheTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // A directory no earlier test of this process used: the cache is
        // process-wide, so a reused path would be served what that test wrote.
        static std::atomic<uint32_t> counter{0};
        m_Root = fs::temp_directory_path() /
                 ("shader_source_cache_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                  std::to_string(counter.fetch_add(1)));
        fs::remove_all(m_Root);
        fs::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    IncludeClosure CollectFor(const std::string& sourceText, const fs::path& source)
    {
        IncludeClosure closure;
        CollectIncludeClosure(sourceText, source, {m_Root}, closure);
        return closure;
    }

    // The cache's entry for `path`, under the key the closure walk uses. The
    // same entry handed back twice is the file served from memory; a new one is
    // the file read again.
    static ShaderSourceCache::EntryPtr EntryFor(const fs::path& path)
    {
        return ShaderSourceCache::Get().Read(path, ShaderSourceCache::NormalizedKey(path));
    }

    fs::path m_Root;
};

TEST_F(ShaderSourceCacheTest, SharedHeaderIsReadOncePerProcessNotOncePerShader)
{
    const std::string text = "// shared by both\n";
    WriteFile(m_Root / "common.glsl", text);
    const std::string shaderA = "#include \"common.glsl\"\nvoid a() {}\n";
    const std::string shaderB = "#include \"common.glsl\"\nvoid b() {}\n";

    const IncludeClosure a = CollectFor(shaderA, m_Root / "a.glsl");
    const ShaderSourceCache::EntryPtr afterFirst = EntryFor(m_Root / "common.glsl");
    ASSERT_EQ(a.paths.size(), 1u);
    ASSERT_NE(afterFirst, nullptr);

    // Nothing is announced, so the second shader's walk must not touch the disk
    // at all: with the header gone, only a read from memory still finds it.
    fs::remove(m_Root / "common.glsl");

    const IncludeClosure b = CollectFor(shaderB, m_Root / "b.glsl");
    ASSERT_EQ(b.paths.size(), 1u) << "the second shader re-resolved or re-read the shared header from disk";
    EXPECT_EQ(b.paths[0], a.paths[0]);
    EXPECT_EQ(b.hashes[0], HashOf(text));
    EXPECT_EQ(EntryFor(m_Root / "common.glsl"), afterFirst) << "the shared header was read again";
}

TEST_F(ShaderSourceCacheTest, TransitiveIncludesAreHeldToo)
{
    WriteFile(m_Root / "leaf.glsl", "// leaf\n");
    WriteFile(m_Root / "middle.glsl", "#include \"leaf.glsl\"\n");
    const std::string shader = "#include \"middle.glsl\"\n";

    const IncludeClosure first = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(first.paths.size(), 2u);
    fs::remove(m_Root / "leaf.glsl");
    fs::remove(m_Root / "middle.glsl");

    const IncludeClosure second = CollectFor(shader, m_Root / "b.glsl");
    EXPECT_EQ(second.paths, first.paths) << "a transitive include was resolved or read from disk again";
    EXPECT_EQ(second.hashes, first.hashes);
}

TEST_F(ShaderSourceCacheTest, AnAnnouncedEditChangesTheKey)
{
    const fs::path header = m_Root / "common.glsl";
    WriteFile(header, "// before\n");
    const std::string shader = "#include \"common.glsl\"\n";

    const IncludeClosure before = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(before.hashes.size(), 1u);

    const std::string after = "// after, a different length entirely\n";
    WriteFile(header, after);
    const IncludeClosure unannounced = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(unannounced.hashes.size(), 1u);
    EXPECT_EQ(unannounced.hashes[0], before.hashes[0]) << "an unannounced edit was read: the cache is not holding";

    // The announcement is what the file watcher's lane sends; after it the
    // next key must fold the bytes on disk, or the compile serves SPIR-V built
    // from content the file no longer has.
    NotifyShaderSourcesChanged();
    const IncludeClosure announced = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(announced.hashes.size(), 1u);
    EXPECT_EQ(announced.hashes[0], HashOf(after)) << "an announced edit kept serving the old content";
}

TEST_F(ShaderSourceCacheTest, AnAnnouncedMtimeAndLengthPreservingEditStillChangesTheKey)
{
    // Same byte count, same mtime, different bytes: the case a timestamp check
    // cannot see. An announcement drops the held bytes outright, so nothing
    // about the file's metadata decides whether it is read again.
    const fs::path header = m_Root / "common.glsl";
    WriteFile(header, "// aaaa\n");
    const std::string shader = "#include \"common.glsl\"\n";

    const IncludeClosure before = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(before.hashes.size(), 1u);
    const auto stamp = fs::last_write_time(header);
    const auto size = fs::file_size(header);

    WriteFile(header, "// bbbb\n");
    fs::last_write_time(header, stamp);
    ASSERT_EQ(fs::file_size(header), size) << "the rewrite must be the same length";
    ASSERT_EQ(fs::last_write_time(header), stamp) << "the rewrite must preserve the mtime";
    NotifyShaderSourcesChanged();

    const IncludeClosure after = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(after.hashes.size(), 1u);
    EXPECT_NE(before.hashes[0], after.hashes[0]) << "an mtime-preserving rewrite slipped past the announcement";
}

TEST_F(ShaderSourceCacheTest, AMissingIncludeAddsNothingToTheKey)
{
    // A directive naming nothing on disk resolves to no file at all, so the
    // closure records nothing for it -- the compile reports it, not the key.
    const std::string shader = "#include \"missing.glsl\"\n";
    const IncludeClosure closure = CollectFor(shader, m_Root / "a.glsl");
    EXPECT_TRUE(closure.paths.empty());
    EXPECT_TRUE(closure.hashes.empty());
}

TEST_F(ShaderSourceCacheTest, AFileThatCannotBeReadIsNotHeld)
{
    // A failed read is not remembered: the file is read when it exists, with
    // no announcement needed for a path the cache never held.
    const fs::path header = m_Root / "later.glsl";
    EXPECT_EQ(EntryFor(header), nullptr);
    WriteFile(header, "// here now\n");
    const ShaderSourceCache::EntryPtr found = EntryFor(header);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->Hash, HashOf("// here now\n"));

    // Deleted and announced, it is gone; recreated, it is read afresh.
    fs::remove(header);
    NotifyShaderSourcesChanged();
    EXPECT_EQ(EntryFor(header), nullptr) << "the entry for the deleted file was kept";
    WriteFile(header, "// back again\n");
    const ShaderSourceCache::EntryPtr back = EntryFor(header);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->Hash, HashOf("// back again\n"));
}

TEST_F(ShaderSourceCacheTest, AnIncludeThatAppearsLaterEntersTheKeyOnceAnnounced)
{
    // An author fixing a missing include by creating the file: the watcher
    // reports the create, and the next walk must find the file, or the program
    // is keyed without it and later edits to it cannot move the key.
    const std::string shader = "#include \"appears_later.glsl\"\n";

    const IncludeClosure missing = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_TRUE(missing.paths.empty());

    WriteFile(m_Root / "appears_later.glsl", "// created after the failed walk\n");
    NotifyShaderSourcesChanged();

    const IncludeClosure found = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(found.paths.size(), 1u) << "a failed resolution outlived the announcement";
    EXPECT_EQ(found.hashes[0], HashOf("// created after the failed walk\n"));
}

TEST_F(ShaderSourceCacheTest, TheSameOperandFromTwoDirectoriesResolvesToEachOwnFile)
{
    // The requesting file's own directory is searched FIRST, so the identical
    // spelling must reach a different file depending on who wrote it. Serving
    // one shader the other's header would build the key from bytes that shader
    // never compiles.
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    WriteFile(m_Root / "common.glsl", "// the root one\n");
    WriteFile(nested / "common.glsl", "// the nested one, shadowing it\n");

    const std::string shader = "#include \"common.glsl\"\n";
    const IncludeClosure fromRoot = CollectFor(shader, m_Root / "a.glsl");
    const IncludeClosure fromNested = CollectFor(shader, nested / "b.glsl");

    ASSERT_EQ(fromRoot.paths.size(), 1u);
    ASSERT_EQ(fromNested.paths.size(), 1u);
    EXPECT_NE(fromRoot.paths[0], fromNested.paths[0]);
    EXPECT_NE(fromRoot.hashes[0], fromNested.hashes[0]) << "one directory's header was served to the other";
}

// The three cases below are what a file appearing or disappearing earlier in a
// search order does to the key once the watcher has reported it: each walk
// reaches the file the compiler reads now.
TEST_F(ShaderSourceCacheTest, AShadowingIncludeTakesOverOnceAnnounced)
{
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    WriteFile(m_Root / "common.glsl", "// the root one\n");

    const std::string shader = "#include \"common.glsl\"\n";
    const IncludeClosure before = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(before.paths.size(), 1u);

    const fs::path shadowing = nested / "common.glsl";
    WriteFile(shadowing, "// now shadowing the root one\n");
    NotifyShaderSourcesChanged();

    const IncludeClosure after = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(after.paths.size(), 1u);
    EXPECT_EQ(fs::path(after.paths[0]), fs::absolute(shadowing).lexically_normal())
        << "the key still folds the file the compiler no longer reads";
    EXPECT_NE(before.hashes[0], after.hashes[0]);
}

TEST_F(ShaderSourceCacheTest, DeletingAShadowingIncludeFallsBackOnceAnnounced)
{
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    const fs::path rootCommon = m_Root / "common.glsl";
    const fs::path shadowing = nested / "common.glsl";
    WriteFile(rootCommon, "// the root one\n");
    WriteFile(shadowing, "// shadowing the root one\n");

    const std::string shader = "#include \"common.glsl\"\n";
    ASSERT_EQ(CollectFor(shader, nested / "b.glsl").paths.size(), 1u);

    fs::remove(shadowing);
    NotifyShaderSourcesChanged();
    const IncludeClosure afterDelete = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(afterDelete.paths.size(), 1u);
    EXPECT_EQ(fs::path(afterDelete.paths[0]), fs::absolute(rootCommon).lexically_normal())
        << "the key is stuck on a file that no longer exists";

    // And the file it fell back to still moves the key when edited.
    WriteFile(rootCommon, "// the root one, edited\n");
    NotifyShaderSourcesChanged();
    const IncludeClosure afterEdit = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(afterEdit.hashes.size(), 1u);
    EXPECT_NE(afterDelete.hashes[0], afterEdit.hashes[0]);
}

TEST_F(ShaderSourceCacheTest, AWarmWalkKeysWhatAFreshWalkOfTheSameDiskWould)
{
    // After any announced history of shadowing files coming and going, the
    // closure is the one a process that never saw that history computes from
    // the disk alone: the cook and the runtime must agree on the key.
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    const fs::path rootCommon = m_Root / "common.glsl";
    const fs::path shadowing = nested / "common.glsl";
    const std::string shader = "#include \"common.glsl\"\n";
    WriteFile(rootCommon, "// the root one\n");
    CollectFor(shader, nested / "b.glsl");
    WriteFile(shadowing, "// shadowing\n");
    NotifyShaderSourcesChanged();
    CollectFor(shader, nested / "b.glsl");
    fs::remove(shadowing);
    NotifyShaderSourcesChanged();
    CollectFor(shader, nested / "b.glsl");
    const std::string finalText = "// the root one, final\n";
    WriteFile(rootCommon, finalText);
    NotifyShaderSourcesChanged();

    const IncludeClosure warm = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(warm.paths.size(), 1u);
    EXPECT_EQ(fs::path(warm.paths[0]), fs::absolute(rootCommon).lexically_normal());
    EXPECT_EQ(warm.hashes[0], HashOf(finalText));
}

TEST_F(ShaderSourceCacheTest, ClosureContentIsIdenticalWarmAndCold)
{
    WriteFile(m_Root / "leaf.glsl", "// leaf\n");
    WriteFile(m_Root / "middle.glsl", "#include \"leaf.glsl\"\n");
    const std::string shader = "#include \"middle.glsl\"\n#include \"leaf.glsl\"\n";

    const IncludeClosure cold = CollectFor(shader, m_Root / "a.glsl");
    const IncludeClosure warm = CollectFor(shader, m_Root / "a.glsl");

    // The cache must not change what the key is built from -- only how often
    // the bytes behind it are read.
    ASSERT_EQ(cold.paths, warm.paths);
    ASSERT_EQ(cold.hashes, warm.hashes);
}

TEST_F(ShaderSourceCacheTest, ConcurrentWalksAndAnnouncementsStayConsistent)
{
    // The prewarm walks closures on every job worker while the watcher thread
    // announces edits. Every walk must see each header whole, as one of the
    // versions it ever had -- never a torn read, a lost include or a crash --
    // and once the last edit is announced, every walk folds the final bytes.
    constexpr int kHeaders = 6;
    std::string shader;
    for (int h = 0; h < kHeaders; ++h)
    {
        const std::string name = "shared_" + std::to_string(h) + ".glsl";
        WriteFile(m_Root / name, "// version 0 of " + name + "\n");
        shader += "#include \"" + name + "\"\n";
    }
    constexpr int kVersions = 40;
    std::vector<std::vector<uint64_t>> validHashes(kHeaders);
    for (int h = 0; h < kHeaders; ++h)
        for (int v = 0; v < kVersions; ++v)
            validHashes[h].push_back(HashOf(SharedHeaderVersion(h, v)));

    ConcurrentWalks walks{shader, m_Root, validHashes};
    constexpr int kWalkers = 8;
    std::vector<std::thread> walkers;
    for (int t = 0; t < kWalkers; ++t)
        walkers.emplace_back(WalkUntilDone, std::ref(walks));
    // Each edit is a write to a temporary sibling renamed over the header, the
    // way editors save: a reader sees the old file or the new one, never half.
    for (int v = 1; v < kVersions; ++v)
    {
        for (int h = 0; h < kHeaders; ++h)
        {
            const fs::path temp = m_Root / ("shared_" + std::to_string(h) + ".tmp");
            WriteFile(temp, SharedHeaderVersion(h, v));
            RenameOver(temp, m_Root / ("shared_" + std::to_string(h) + ".glsl"));
        }
        NotifyShaderSourcesChanged();
    }
    walks.Done.store(true, std::memory_order_release);
    for (std::thread& t : walkers)
        t.join();

    EXPECT_GT(walks.Walks.load(), 0);
    EXPECT_EQ(walks.Torn.load(), 0) << "a concurrent walk folded a header that was never on disk, or lost one";

    const IncludeClosure settled = CollectFor(shader, m_Root / "after.glsl");
    ASSERT_EQ(settled.hashes.size(), static_cast<size_t>(kHeaders));
    for (int h = 0; h < kHeaders; ++h)
        EXPECT_EQ(settled.hashes[h], validHashes[h].back()) << "header " << h << " kept a version from before the last announcement";
}

TEST_F(ShaderSourceCacheTest, AReadInFlightAcrossAnAnnouncementIsNotHeld)
{
    // A worker still reading the old bytes when the watcher announces an edit
    // must not store them afterwards, or every later key keeps folding a file
    // that is no longer on disk. The header is large so the read is still in
    // flight when the announcement lands.
    constexpr size_t kHeaderBytes = 4u << 20;
    constexpr int kAttempts = 8;
    const fs::path header = m_Root / "large.glsl";
    int stale = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        const std::string before(kHeaderBytes, static_cast<char>('a' + attempt % 13));
        const std::string after(kHeaderBytes, static_cast<char>('n' + attempt % 13));
        WriteFile(header, before);
        WriteFile(m_Root / "large.tmp", after);
        NotifyShaderSourcesChanged();

        std::atomic<bool> started{false};
        std::thread reader([&]() {
            started.store(true, std::memory_order_release);
            EntryFor(header);
        });
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::this_thread::sleep_for(kReaderHeadStart);
        RenameOver(m_Root / "large.tmp", header);
        NotifyShaderSourcesChanged();
        reader.join();

        const ShaderSourceCache::EntryPtr held = EntryFor(header);
        ASSERT_NE(held, nullptr);
        if (held->Hash != HashOf(after))
            ++stale;
    }
    EXPECT_EQ(stale, 0) << "a read that started before the announcement was stored after it";
}

TEST_F(ShaderSourceCacheTest, AFailedResolutionInFlightAcrossACreateIsNotHeld)
{
    // The twin of the read case: a walk that found no file, still searching the
    // later roots when the file is created and announced, must not store its
    // "resolves nowhere". The created file's directory is searched first and a
    // long list of empty roots after it keeps the search in flight.
    constexpr int kEmptyRoots = 4000;
    const fs::path appearing = m_Root / "appearing";
    fs::create_directories(appearing);
    std::vector<fs::path> roots{appearing};
    for (int r = 0; r < kEmptyRoots; ++r)
        roots.push_back(m_Root / ("empty_" + std::to_string(r)));
    const std::string signature = ShaderSourceCache::MakeRootsSignature(roots);
    const std::string operand = "created_later.glsl";

    std::atomic<bool> started{false};
    std::thread walker([&]() {
        started.store(true, std::memory_order_release);
        ShaderSourceCache::Get().Resolve({}, roots, signature, operand);
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(kReaderHeadStart);
    WriteFile(appearing / operand, "// created while a walk was searching for it\n");
    NotifyShaderSourcesChanged();
    walker.join();

    const ShaderSourceCache::ResolutionPtr resolved = ShaderSourceCache::Get().Resolve({}, roots, signature, operand);
    ASSERT_NE(resolved, nullptr) << "a failed resolution that started before the announcement was stored after it";
    EXPECT_EQ(resolved->Key, ShaderSourceCache::NormalizedKey(appearing / operand));
}
} // namespace
