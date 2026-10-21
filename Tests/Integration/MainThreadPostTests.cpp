// MainThread.Post's native half and the main-thread mark it rests on: EngineCore marks the thread that
// initializes it on every configuration, and GE_MainThread_RequestPump reaches the main-thread pump only
// through EngineCore's post gate, which answers queued, run inline or dropped.
//
// The managed queue lives in Scripting.ABI; PostQueue below follows its protocol exactly (the request and
// the append under one lock, an inline run outside it, a drain that swaps the batch out and runs it) so these
// arms drive the real exports, the real gate and the real pump without a CLR. The managed queue's own rules
// (the unload drop) are pinned in InteropTests.

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "EngineLogCapture.h"
#include "PluginAPI/EnginePlugin.h"
#include "Scripting/ScriptingABI.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using GameEngine::EngineCore;

// The managed MainThread.Post queue's protocol, in C++: what these arms post through.
class PostQueue
{
  public:
    // MainThread.Post: the request and the append under the queue's lock, an inline run outside it.
    int32_t Post(std::function<void()> action)
    {
        int32_t answer = GE_MainThreadPost_Dropped;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            answer = GE_MainThread_RequestPump();
            if (answer == GE_MainThreadPost_Queued)
            {
                m_Queue.push_back(std::move(action));
                return answer;
            }
        }
        if (answer == GE_MainThreadPost_RunInline)
            action();
        return answer;
    }

    // The drain: swap the batch out under the lock, run it outside.
    void TakeAndRun(bool run)
    {
        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            batch.swap(m_Queue);
        }
        if (!run)
            return;
        for (std::function<void()>& action : batch)
            action();
    }

  private:
    std::mutex m_Mutex;
    std::vector<std::function<void()>> m_Queue;
};

PostQueue& Posts()
{
    static PostQueue s_Posts;
    return s_Posts;
}

void GE_CDECL Drain(int32_t run)
{
    Posts().TakeAndRun(run != 0);
}

// The drain is installed once per process, first-wins, as Scripting.ABI installs its own.
void InstallDrain()
{
    static const GE_Result s_Installed = GE_MainThread_SetDrain(&Drain);
    ASSERT_EQ(s_Installed, GE_Result_Ok);
}

GameEngine::ScriptsConfig NativeAotPlayerScripts()
{
    return GameEngine::BuildPlayerScriptsConfig(GameEngine::PackagedGameLayout{}, /*useNativeAOT=*/true);
}

// A plugin whose component registration throws: EngineCore::Initialize fails after its scripting block.
class ThrowingPlugin final : public GameEngine::Plugins::IEnginePlugin
{
  public:
    const GameEngine::Plugins::PluginDescriptor& GetDescriptor() const override { return m_Descriptor; }
    void RegisterEngineComponents() override { throw std::runtime_error("component registration failed"); }

  private:
    GameEngine::Plugins::PluginDescriptor m_Descriptor{"MainThreadPostTest.ThrowingPlugin", "Throwing", "1.0", true};
};

// Every arm starts from a shut-down engine and leaves one: the gate state each arm asserts is the one it
// set up. The engine runs the NativeAOT Player's scripts configuration, CLR off, unless an arm says
// otherwise.
class MainThreadPostTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        EngineCore::GetInstance().Shutdown();
        ASSERT_NO_FATAL_FAILURE(InstallDrain());
        Posts().TakeAndRun(false);
    }

    void TearDown() override { EngineCore::GetInstance().Shutdown(); }

    // Initializes the engine on this thread, which marks it as the main thread.
    bool Initialize(const GameEngine::ScriptsConfig& scripts = NativeAotPlayerScripts(), bool standaloneHosted = false)
    {
        GameEngine::ApplicationConfig config{};
        config.WorkspaceDirectory = m_Workspace.Path().string();
        config.AssetDirectory = "Assets";
        config.StandaloneHosted = standaloneHosted;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        return EngineCore::GetInstance().Initialize(config);
    }

    static void Pump() { EngineCore::GetInstance().GetScriptManager().ProcessMainThreadTasks(); }

    // Fails an Initialize after its scripting block (a plugin's component registration throws), posts from
    // another thread, writes the answer to stderr and exits the process.
    void PostAfterAFailedInitialize()
    {
        ThrowingPlugin plugin;
        GameEngine::Plugins::EnginePluginRegistry::Get().RegisterPlugin(plugin);
        bool threw = false;
        try
        {
            (void)Initialize();
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        const int32_t answer = threw ? PostFromAnotherThread([] {}) : -1;
        std::fprintf(stderr, "post answered %d\n", answer);
        std::_Exit(0);
    }

    // Posts from a thread of this test's own and returns the answer.
    static int32_t PostFromAnotherThread(std::function<void()> action)
    {
        int32_t answer = -1;
        std::thread poster([&] { answer = Posts().Post(std::move(action)); });
        poster.join();
        return answer;
    }

  private:
    GameEngine::TestUtils::ScopedTempDir m_Workspace{GameEngine::TestUtils::MakeUniqueTempDirectory("MainThreadPost")};
};

// Records the order actions ran in and whether each ran on the engine's main thread.
struct RunLog
{
    void Record(int id)
    {
        std::lock_guard<std::mutex> lock(Mutex);
        Ids.push_back(id);
        OnMainThread.push_back(EngineCore::GetInstance().IsMainThread());
    }
    std::vector<int> TakeIds()
    {
        std::lock_guard<std::mutex> lock(Mutex);
        return Ids;
    }
    std::mutex Mutex;
    std::vector<int> Ids;
    std::vector<bool> OnMainThread;
};

GameEngine::ScriptsConfig ScriptingOff()
{
    GameEngine::ScriptsConfig scripts{};
    scripts.disableClr = true;
    scripts.enableHotReload = false;
    scripts.enableAsyncHotReload = false;
    scripts.enableAutoProjectGeneration = false;
    return scripts;
}
} // namespace

// The NativeAOT Player runs scripts with the CLR off, and its engine still marks the main thread and keeps a
// ScriptManager for the main-thread queue.
TEST_F(MainThreadPostTest, NativeAotPlayerConfigurationMarksTheMainThread)
{
    ASSERT_TRUE(Initialize());
    const EngineCore& engine = EngineCore::GetInstance();
    EXPECT_TRUE(engine.HasEverInitialized());
    EXPECT_TRUE(engine.IsMainThread());
    EXPECT_FALSE(engine.IsStandaloneHosted());
    EXPECT_TRUE(engine.HasScriptManagerForTests()) << "the queue a post reaches";
    bool otherThreadIsMain = true;
    std::thread other([&] { otherThreadIsMain = EngineCore::GetInstance().IsMainThread(); });
    other.join();
    EXPECT_FALSE(otherThreadIsMain);
}

TEST_F(MainThreadPostTest, MainThreadPostRunsOnTheFrameThreadInOrder)
{
    ASSERT_TRUE(Initialize());
    RunLog log;
    std::thread worker([&] {
        EXPECT_EQ(Posts().Post([&] { log.Record(1); }), GE_MainThreadPost_Queued);
        EXPECT_EQ(Posts().Post([&] { log.Record(2); }), GE_MainThreadPost_Queued);
    });
    worker.join();
    EXPECT_TRUE(log.TakeIds().empty()) << "nothing runs before the pump";

    Pump();
    EXPECT_EQ(log.TakeIds(), (std::vector<int>{1, 2}));
    EXPECT_EQ(log.OnMainThread, (std::vector<bool>{true, true}));
}

TEST_F(MainThreadPostTest, MainThreadPostFromAForeignThreadRunsAtThePumpInTheNativeAotConfiguration)
{
    ASSERT_TRUE(Initialize());
    RunLog log;
    EXPECT_EQ(PostFromAnotherThread([&] { log.Record(1); }), GE_MainThreadPost_Queued);
    EXPECT_TRUE(log.TakeIds().empty()) << "the action had not run when the post returned";

    Pump();
    EXPECT_EQ(log.TakeIds(), (std::vector<int>{1}));
    EXPECT_EQ(log.OnMainThread, (std::vector<bool>{true})) << "it ran on the initializing thread";
}

// The drain is one step per pump, ahead of the task loop and outside its cap: a flood of posts runs in one
// batch while the cap holds back native tasks only.
TEST_F(MainThreadPostTest, OneDrainStepPerPumpWhateverThePostCount)
{
    constexpr int kNativeTasks = 11;
    constexpr int kPosts = 1000;
    ASSERT_TRUE(Initialize());
    std::atomic<int> nativeRan{0};
    for (int i = 0; i < kNativeTasks; ++i)
        EngineCore::GetInstance().GetScriptManager().QueueMainThreadTask([&] { ++nativeRan; });
    RunLog log;
    std::thread worker([&] {
        for (int i = 0; i < kPosts; ++i)
            Posts().Post([&log, i] { log.Record(i); });
    });
    worker.join();

    Pump();
    const std::vector<int> ids = log.TakeIds();
    ASSERT_EQ(ids.size(), static_cast<size_t>(kPosts)) << "every post ran at this pump";
    for (int i = 0; i < kPosts; ++i)
        ASSERT_EQ(ids[static_cast<size_t>(i)], i) << "in the order they were posted";
    EXPECT_EQ(nativeRan.load(), kNativeTasks - 1) << "the task cap holds back the eleventh native task only";
}

// A post that lands while the drain runs its batch is in the next batch: never stranded, never in this one.
TEST_F(MainThreadPostTest, APostDuringTheDrainRunsAtTheNextPump)
{
    ASSERT_TRUE(Initialize());
    RunLog log;
    EXPECT_EQ(PostFromAnotherThread([&] {
                  log.Record(1);
                  EXPECT_EQ(PostFromAnotherThread([&] { log.Record(2); }), GE_MainThreadPost_Queued);
              }),
              GE_MainThreadPost_Queued);

    Pump();
    EXPECT_EQ(log.TakeIds(), (std::vector<int>{1})) << "the post made during the drain waits for the next pump";
    Pump();
    EXPECT_EQ(log.TakeIds(), (std::vector<int>{1, 2}));
}

TEST_F(MainThreadPostTest, ASelfRepostingActionRunsOncePerPump)
{
    ASSERT_TRUE(Initialize());
    std::atomic<int> runs{0};
    std::function<void()> repost = [&] {
        ++runs;
        Posts().Post(repost);
    };
    EXPECT_EQ(PostFromAnotherThread(repost), GE_MainThreadPost_Queued);

    for (int pump = 1; pump <= 3; ++pump)
    {
        Pump();
        EXPECT_EQ(runs.load(), pump) << "one run per pump";
    }
    EngineCore::GetInstance().Shutdown();
}

// A re-initialized engine starts with an empty queue and a clear request: the first engine's unpumped post is
// discarded at its Shutdown, and the second engine's post runs at its first pump.
TEST_F(MainThreadPostTest, AReinitializedEnginesFirstPostRuns)
{
    RunLog log;
    ASSERT_TRUE(Initialize());
    EXPECT_EQ(PostFromAnotherThread([&] { log.Record(1); }), GE_MainThreadPost_Queued);
    EngineCore::GetInstance().Shutdown();

    ASSERT_TRUE(Initialize());
    EXPECT_EQ(PostFromAnotherThread([&] { log.Record(2); }), GE_MainThreadPost_Queued);
    Pump();
    EXPECT_EQ(log.TakeIds(), (std::vector<int>{2}));
}

TEST_F(MainThreadPostTest, PostsQueuedAtShutdownAreDiscardedNotRun)
{
    RunLog log;
    ASSERT_TRUE(Initialize());
    EXPECT_EQ(PostFromAnotherThread([&] { log.Record(1); }), GE_MainThreadPost_Queued);
    EXPECT_EQ(PostFromAnotherThread([&] { log.Record(2); }), GE_MainThreadPost_Queued);
    EngineCore::GetInstance().Shutdown();
    EXPECT_TRUE(log.TakeIds().empty()) << "Shutdown discards the queue rather than running it";

    ASSERT_TRUE(Initialize());
    Pump();
    EXPECT_TRUE(log.TakeIds().empty()) << "nor does a later engine's pump run it";
}

// The drop is logged once per process, so an earlier drop in the process may already have written the line.
TEST_F(MainThreadPostTest, APostAfterEngineShutdownIsDropped)
{
    ASSERT_TRUE(Initialize());
    EngineCore::GetInstance().Shutdown();
    std::vector<std::string> lines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&lines);

    RunLog log;
    EXPECT_EQ(PostFromAnotherThread([&] { log.Record(1); }), GE_MainThreadPost_Dropped);
    Logger::Log::Flush();
    EXPECT_TRUE(log.TakeIds().empty());
    EXPECT_LE(GameEngine::TestLog::CountLinesContaining(lines, "MainThread.Post: the engine has shut down"), 1u);
    EXPECT_FALSE(EngineCore::GetInstance().HasScriptManagerForTests()) << "a post never constructs a ScriptManager";
}

// An Initialize that fails after it opened the post gate closes it again: a post made before the next Initialize
// is dropped rather than queued on a manager nothing pumps. The arm runs in a child process: a failed Initialize
// leaves the process's engine unable to initialize again, and the arms after this one need it.
TEST_F(MainThreadPostTest, APostAfterAFailedInitializeIsDroppedNotQueued)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(PostAfterAFailedInitialize(), ::testing::ExitedWithCode(0), "post answered 2");
}

// An engine initialized with scripting off has no main-thread queue, and answers the same whether or not an
// earlier engine of the process shut down: a post runs at once, on the posting thread.
TEST_F(MainThreadPostTest, AnEngineWithScriptingOffRunsAPostInlineAfterAnEarlierShutdown)
{
    ASSERT_TRUE(Initialize());
    EngineCore::GetInstance().Shutdown();
    ASSERT_TRUE(Initialize(ScriptingOff()));
    EXPECT_FALSE(EngineCore::GetInstance().HasScriptManagerForTests());

    std::thread::id ranOn;
    std::thread::id postedFrom;
    std::thread poster([&] {
        postedFrom = std::this_thread::get_id();
        EXPECT_EQ(Posts().Post([&] { ranOn = std::this_thread::get_id(); }), GE_MainThreadPost_RunInline);
    });
    poster.join();
    EXPECT_EQ(ranOn, postedFrom) << "the action had run, on the posting thread, when the post returned";
}

// A standalone-hosted engine (what the scripting ABI's bootstrap initializes for dotnet test) has no frame loop
// and no pump: a post runs at once, on the posting thread.
TEST_F(MainThreadPostTest, APostInAStandaloneHostedEngineRunsInline)
{
    GameEngine::ScriptsConfig scripts{};
    scripts.enableHotReload = false;
    scripts.disableClr = true;
    scripts.nativeAot = true;
    ASSERT_TRUE(Initialize(scripts, /*standaloneHosted=*/true));
    ASSERT_TRUE(EngineCore::GetInstance().IsStandaloneHosted());

    std::thread::id ranOn;
    std::thread::id postedFrom;
    std::thread poster([&] {
        postedFrom = std::this_thread::get_id();
        EXPECT_EQ(Posts().Post([&] { ranOn = std::this_thread::get_id(); }), GE_MainThreadPost_RunInline);
        EXPECT_EQ(ranOn, postedFrom) << "the action had run, on the posting thread, when the post returned";
    });
    poster.join();
}

// One thread posts in a loop while this thread shuts the engine down and initializes it again: every post is
// answered, and none reaches a ScriptManager a Shutdown freed (run under ASan to see a use after free).
TEST_F(MainThreadPostTest, APostLoopAcrossShutdownAndReinitializeIsClean)
{
    constexpr int kCycles = 10;
    std::atomic<bool> stop{false};
    std::atomic<int> answered{0};
    std::atomic<int> unexpected{0};
    std::thread poster([&] {
        while (!stop.load())
        {
            const int32_t answer = Posts().Post([] {});
            const bool known = answer == GE_MainThreadPost_Queued || answer == GE_MainThreadPost_RunInline ||
                               answer == GE_MainThreadPost_Dropped;
            ++(known ? answered : unexpected);
            // Paced, and the loop still crosses every Shutdown and Initialize.
            std::this_thread::sleep_for(100us);
        }
    });
    // No fatal assertion while the poster is joinable: it would return with the thread unjoined, and the
    // std::thread destructor would terminate the process instead of reporting.
    bool initialized = true;
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        initialized = Initialize();
        if (!initialized)
            break;
        Pump();
        EngineCore::GetInstance().Shutdown();
    }
    stop.store(true);
    poster.join();
    EXPECT_TRUE(initialized);
    EXPECT_GT(answered.load(), 0);
    EXPECT_EQ(unexpected.load(), 0);
}
