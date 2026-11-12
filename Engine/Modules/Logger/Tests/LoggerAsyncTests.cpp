#include <gtest/gtest.h>

#include "Logger/Logger.h"
#include "Logger/CallbackSink.h"
#include "Logger/FileSink.h"
#include "Logger/RingBufferSink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{

using namespace Logger;

// Fixture: initializes Logger with a RingBufferSink, shuts down in TearDown.
class LoggerAsyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Config cfg;
        cfg.GlobalMinLevel = LogLevel::Trace;
        cfg.EnableSourceLocation = false;
        Log::Initialize(cfg);

        Log::ClearSinks();

        auto sink = std::make_unique<RingBufferSink>(50000);
        m_Spy = sink.get();
        Log::AddSink(std::move(sink));
    }

    void TearDown() override
    {
        Log::Shutdown();
        m_Spy = nullptr;
    }

    RingBufferSink* m_Spy = nullptr;
};

// ==========================================================================
// A) CORRECTNESS
// ==========================================================================

TEST_F(LoggerAsyncTest, SourceMacrosKeepCallerFunctionAndLine)
{
    auto captured = std::make_shared<std::vector<LogMessage>>();
    auto sink = std::make_unique<CallbackSink>();
    sink->RegisterCallback([captured](const LogMessage& message) { captured->push_back(message); });
    Log::AddSink(std::move(sink));
    const int expectedLine = __LINE__ + 1;
    LOG_INFO("macro {}", 3);
    LOG_CRITICAL("critical {}", 4);
    Log::Flush();
    const auto& messages = *captured;
    ASSERT_EQ(messages.size(), 2u);
    for (const LogMessage& message : messages)
    {
        EXPECT_EQ(message.Channel, CoreLog().Index);
        EXPECT_EQ(message.SourceFile, __FILE__);
        EXPECT_TRUE(message.Function.ends_with("TestBody")) << message.Function;
    }
    EXPECT_EQ(messages[0].SourceLine, expectedLine);
    EXPECT_EQ(messages[1].SourceLine, expectedLine + 1);
}

TEST_F(LoggerAsyncTest, FilteredSourceMacroDoesNotEvaluateArguments)
{
    Log::SetLogLevel(LogLevel::Critical);
    int evaluations = 0;
    LOG_INFO("filtered {}", ++evaluations);
    EXPECT_EQ(evaluations, 0);
}

TEST_F(LoggerAsyncTest, BasicMessageDelivery)
{
    constexpr int kCount = 50;
    for (int i = 0; i < kCount; ++i)
    {
        Log::Info("message {}", i);
    }
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kCount));
}

TEST_F(LoggerAsyncTest, MessageOrdering_SingleThread)
{
    Log::Info("alpha");
    Log::Info("bravo");
    Log::Info("charlie");
    Log::Flush();

    auto msgs = m_Spy->GetMessages({}).Messages;
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_NE(msgs[0].Message.find("alpha"), std::string::npos);
    EXPECT_NE(msgs[1].Message.find("bravo"), std::string::npos);
    EXPECT_NE(msgs[2].Message.find("charlie"), std::string::npos);
}

TEST_F(LoggerAsyncTest, FlushBlocksUntilDelivered)
{
    for (int i = 0; i < 200; ++i)
    {
        Log::Info("msg {}", i);
    }
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), 200u);
}

TEST_F(LoggerAsyncTest, ShutdownDrainsRemaining)
{
    // Use an atomic counter via CallbackSink to survive Shutdown (which
    // destroys sinks, making m_Spy a dangling pointer).
    std::atomic<int> messageCount{0};
    auto callbackSink = std::make_unique<CallbackSink>();
    callbackSink->RegisterCallback([&messageCount](const LogMessage&)
    {
        messageCount.fetch_add(1, std::memory_order_relaxed);
    });
    Log::AddSink(std::move(callbackSink));

    for (int i = 0; i < 100; ++i)
    {
        Log::Info("drain-check {}", i);
    }
    // No Flush -- Shutdown must drain.
    Log::Shutdown();

    // Counter lives on the stack, safe to access after Shutdown.
    EXPECT_EQ(messageCount.load(), 100);

    // Re-initialize so TearDown doesn't double-shutdown.
    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

#if LOGGER_ENABLE_FILE_LOGGING
// A burst's trailing lines must become visible WITHOUT an explicit Flush():
// FileSink defaults to autoFlush=false, so before the drain thread's bounded
// idle flush they could sit in the stream buffer until unrelated later traffic
// happened to fill it (observed as build-completion lines arriving minutes
// late, masquerading as hangs).
TEST_F(LoggerAsyncTest, BurstTrailingLinesFlushWithoutExplicitFlush)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "ge_logger_burst_flush.txt";
    std::error_code ec;
    fs::remove(path, ec);

    FileSink::Config cfg;
    cfg.filename = path.string().c_str();
    cfg.append = false;
    ASSERT_FALSE(cfg.autoFlush) << "test assumes the buffered default";
    Log::AddSink(std::make_unique<FileSink>(cfg));

    // Small burst (well under the stream buffer size) ending in a sentinel, so
    // nothing self-flushes by filling the buffer.
    for (int i = 0; i < 5; ++i)
    {
        Log::Info("burst line {}", i);
    }
    Log::Info("BURST-SENTINEL-DONE");

    // Deliberately NO Log::Flush() — the drain's bounded idle flush must
    // surface the sentinel on its own. Generous ceiling for CI scheduling; the
    // configured bound is kMaxUnflushedSinkLatency (200ms).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool visible = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::ifstream in(path);
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        if (content.find("BURST-SENTINEL-DONE") != std::string::npos)
        {
            visible = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_TRUE(visible) << "unflushed sink output was not surfaced within the bounded latency";

    // Release the file handle before removing the temp file.
    Log::ClearSinks();
    fs::remove(path, ec);
}

// Log::Flush() must be a barrier all the way to the file, not just to the sink:
// the crash handlers call it and then abort, so there is no bounded idle flush
// left to run and no sink destructor left to close the stream.
TEST_F(LoggerAsyncTest, ExplicitFlushReachesTheFileImmediately)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "ge_logger_explicit_flush.txt";
    std::error_code ec;
    fs::remove(path, ec);

    FileSink::Config cfg;
    cfg.filename = path.string().c_str();
    cfg.append = false;
    ASSERT_FALSE(cfg.autoFlush) << "test assumes the buffered default";
    Log::AddSink(std::make_unique<FileSink>(cfg));

    for (int i = 0; i < 5; ++i)
    {
        Log::Info("pre-flush line {}", i);
    }
    Log::Info("EXPLICIT-FLUSH-SENTINEL");

    Log::Flush();

    // Read straight back: no polling and no sleep, or the bounded idle flush
    // would satisfy the assertion on its own and the barrier would go untested.
    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("EXPLICIT-FLUSH-SENTINEL"), std::string::npos)
        << "Flush() returned with the sink's tail still buffered";
    EXPECT_NE(content.find("pre-flush line 0"), std::string::npos);

    Log::ClearSinks();
    fs::remove(path, ec);
}
#endif // LOGGER_ENABLE_FILE_LOGGING

// With a level set and no Initialize, each line goes to std::cout as "[LEVEL] message".
TEST(LoggerAsyncPreInitTest, LineBeforeInitializeGoesToStdout)
{
    const LogLevel previousLevel = Log::GetLogLevel();
    Log::SetLogLevel(LogLevel::Info);
    std::stringbuf capture;
    std::streambuf* const previousBuffer = std::cout.rdbuf(&capture);

    Log::Info("pre-init message");
    Log::Warning("another pre-init message");

    std::cout.rdbuf(previousBuffer);
    Log::SetLogLevel(previousLevel);
    EXPECT_EQ(capture.str(), "[INFO] pre-init message\n[WARN] another pre-init message\n");
}

// Records the text std::cout receives. The first write of each thread waits, for at most kFirstWriteWait,
// until a second thread writes: without a lock, the second write then starts while the first is in progress.
class InterleaveProbeBuffer : public std::streambuf
{
  public:
    std::string GetText() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Text;
    }

    bool SawOverlappingWrites() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_SawOverlap;
    }

    // Returns false when no thread writes within kFirstWriteArrivalWait.
    bool WaitForFirstWrite()
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_WriteArrived.wait_for(lock, kFirstWriteArrivalWait, [this] { return !m_Text.empty(); });
    }

  protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        Record(std::string_view(data, static_cast<std::size_t>(count)));
        return count;
    }

    int_type overflow(int_type ch) override
    {
        if (!traits_type::eq_int_type(ch, traits_type::eof()))
        {
            const char c = traits_type::to_char_type(ch);
            Record(std::string_view(&c, 1));
        }
        return traits_type::not_eof(ch);
    }

  private:
    static constexpr std::chrono::seconds kFirstWriteWait{1};
    static constexpr std::chrono::seconds kFirstWriteArrivalWait{5};

    void Record(std::string_view piece)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_SawOverlap = m_SawOverlap || m_WritesInProgress > 0;
        ++m_WritesInProgress;
        m_Text.append(piece);
        const bool firstWrite = m_Writers.insert(std::this_thread::get_id()).second;
        m_WriteArrived.notify_all();
        if (firstWrite)
            m_WriteArrived.wait_for(lock, kFirstWriteWait, [this] { return m_Writers.size() >= 2; });
        --m_WritesInProgress;
    }

    mutable std::mutex m_Mutex;
    std::condition_variable m_WriteArrived;
    std::unordered_set<std::thread::id> m_Writers;
    std::string m_Text;
    int m_WritesInProgress = 0;
    bool m_SawOverlap = false;
};

void LogLinesBeforeStart(int thread, int lineCount)
{
    for (int line = 0; line < lineCount; ++line)
    {
        // Each thread starts on a different fallback site: PushEntry, CriticalImpl or DispatchCritical.
        const int site = (thread + line) % 3;
        if (site == 0)
            Log::Info("thread {} line {}", thread, line);
        else if (site == 1)
            Log::Critical("thread {} line {}", thread, line);
        else
            LOG_CRITICAL("thread {} line {}", thread, line);
    }
}

TEST(LoggerAsyncPreInitTest, LinesFromSeveralThreadsArriveWhole)
{
    constexpr int kThreads = 4;
    constexpr int kLinesPerThread = 20;
    const LogLevel previousLevel = Log::GetLogLevel();
    Log::SetLogLevel(LogLevel::Info);
    InterleaveProbeBuffer probe;
    std::streambuf* const previousBuffer = std::cout.rdbuf(&probe);

    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread)
        threads.emplace_back(LogLinesBeforeStart, thread, kLinesPerThread);
    for (auto& thread : threads)
        thread.join();

    std::cout.rdbuf(previousBuffer);
    Log::SetLogLevel(previousLevel);

    std::vector<std::string> expected;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        for (int line = 0; line < kLinesPerThread; ++line)
            expected.push_back(std::format("[{}] thread {} line {}", (thread + line) % 3 == 0 ? "INFO" : "CRITICAL", thread, line));
    }
    std::vector<std::string> received;
    std::istringstream text(probe.GetText());
    for (std::string line; std::getline(text, line);)
        received.push_back(line);
    std::sort(expected.begin(), expected.end());
    std::sort(received.begin(), received.end());
    EXPECT_EQ(received, expected);
    EXPECT_FALSE(probe.SawOverlappingWrites());
}

void LogOneLineBeforeStart()
{
    Log::Info("line before Initialize");
}

// The probe holds a fallback line while this thread starts the logger and logs. Initialize waits until the line
// ends, so the line does not overlap the console sink's line.
TEST(LoggerAsyncPreInitTest, InitializeWaitsForTheFallbackLineInProgress)
{
    const LogLevel previousLevel = Log::GetLogLevel();
    Log::SetLogLevel(LogLevel::Info);
    InterleaveProbeBuffer probe;
    std::streambuf* const previousBuffer = std::cout.rdbuf(&probe);

    std::thread writer(LogOneLineBeforeStart);
    const bool fallbackLineArrived = probe.WaitForFirstWrite();
    if (fallbackLineArrived)
    {
        Log::Config config;
        config.GlobalMinLevel = LogLevel::Info;
        config.EnableSourceLocation = false;
        Log::Initialize(config);
        Log::Info("line after Initialize");
    }
    writer.join();
    Log::Shutdown();

    std::cout.rdbuf(previousBuffer);
    Log::SetLogLevel(previousLevel);

    ASSERT_TRUE(fallbackLineArrived) << "the logger was initialized when the test started";
    const std::string text = probe.GetText();
    EXPECT_FALSE(probe.SawOverlappingWrites());
    EXPECT_TRUE(text.starts_with("[INFO] line before Initialize\n")) << text;
    EXPECT_NE(text.find("line after Initialize"), std::string::npos) << text;
}

TEST_F(LoggerAsyncTest, ReinitAfterShutdown)
{
    Log::Info("before shutdown");
    Log::Shutdown();

    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);

    Log::ClearSinks();
    auto sink = std::make_unique<RingBufferSink>(10000);
    auto* spy2 = sink.get();
    Log::AddSink(std::move(sink));

    Log::Info("after re-init");
    Log::Flush();

    EXPECT_GE(spy2->GetCount(), 1u);
    const auto msgs = spy2->GetMessages({}).Messages;
    EXPECT_EQ(std::count_if(msgs.begin(), msgs.end(), [](const auto& m) { return m.Message.find("after re-init") != std::string::npos; }), 1);
}

// The drain thread can dispatch a record before ClearSinks or Initialize runs.
// The tests repeat each pair so that some rounds meet a queued record.
constexpr int kQueuedRecordRounds = 1000;

TEST_F(LoggerAsyncTest, ClearSinksDeliversQueuedRecordsToTheRemovedSinks)
{
    // ClearSinks destroys the sinks, so the count lives on the heap.
    auto delivered = std::make_shared<std::atomic<int>>(0);
    for (int round = 0; round < kQueuedRecordRounds; ++round)
    {
        auto callbackSink = std::make_unique<CallbackSink>();
        callbackSink->RegisterCallback([delivered](const LogMessage&)
        {
            delivered->fetch_add(1, std::memory_order_relaxed);
        });
        Log::AddSink(std::move(callbackSink));

        Log::Info("logged before ClearSinks {}", round);
        Log::ClearSinks();
    }
    m_Spy = nullptr;

    EXPECT_EQ(delivered->load(), kQueuedRecordRounds);
}

TEST_F(LoggerAsyncTest, InitializeOnARunningLoggerKeepsQueuedRecords)
{
    for (int round = 0; round < kQueuedRecordRounds; ++round)
    {
        Log::Info("logged before Initialize {}", round);
        Log::Initialize({});
    }
    Log::Flush();

    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kQueuedRecordRounds));
}

// Initialize runs in a loop on a second thread, so some rounds call Flush while Initialize runs.
TEST_F(LoggerAsyncTest, FlushRacingInitializeWaitsForItsRecord)
{
    std::atomic<bool> stop{false};
    std::thread initializer([&stop]
    {
        while (!stop.load())
        {
            Log::Initialize({});
        }
    });
    int returnedEarly = 0;
    for (int round = 0; round < kQueuedRecordRounds; ++round)
    {
        const size_t countBefore = m_Spy->GetCount();
        Log::Info("logged before Flush {}", round);
        Log::Flush();
        if (m_Spy->GetCount() <= countBefore)
        {
            ++returnedEarly;
        }
    }
    stop = true;
    initializer.join();

    EXPECT_EQ(returnedEarly, 0);
}

TEST_F(LoggerAsyncTest, CriticalBypassesQueue)
{
    // Critical is dispatched synchronously -- visible without Flush.
    Log::Critical("immediate");

    EXPECT_GE(m_Spy->GetCount(), 1u);
    const auto msgs = m_Spy->GetMessages({.MinLevel = LogLevel::Critical}).Messages;
    EXPECT_EQ(std::count_if(msgs.begin(), msgs.end(), [](const auto& m) { return m.Message.find("immediate") != std::string::npos; }), 1);
}

TEST_F(LoggerAsyncTest, LevelFiltering)
{
    Log::SetLogLevel(LogLevel::Warning);

    Log::Trace("filtered");
    Log::Debug("filtered");
    Log::Info("filtered");
    Log::Warning("visible-warning");
    Log::Error("visible-error");
    Log::Critical("visible-critical");
    Log::Flush();

    EXPECT_EQ(m_Spy->GetCount(), 3u);
    auto msgs = m_Spy->GetMessages({}).Messages;
    for (const auto& m : msgs)
    {
        EXPECT_GE(m.Level, LogLevel::Warning);
    }
}

TEST_F(LoggerAsyncTest, FormatStringWithMultipleArgs)
{
    Log::Info("values: {} {} {} {} {}", 1, 2.5, "hello", 'X', true);
    Log::Flush();

    ASSERT_EQ(m_Spy->GetCount(), 1u);
    auto msgs = m_Spy->GetMessages({}).Messages;
    EXPECT_NE(msgs[0].Message.find("values:"), std::string::npos);
    EXPECT_NE(msgs[0].Message.find("hello"), std::string::npos);
}

TEST_F(LoggerAsyncTest, AllLogLevelsDelivered)
{
    Log::Trace("t");
    Log::Debug("d");
    Log::Info("i");
    Log::Warning("w");
    Log::Error("e");
    Log::Critical("c");
    Log::Flush();

    EXPECT_EQ(m_Spy->GetCount(), 6u);
}

TEST_F(LoggerAsyncTest, EmptyMessage_NoCrash)
{
    Log::Info("");
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), 1u);
}

TEST_F(LoggerAsyncTest, FlushOnEmptyQueue_NoCrash)
{
    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), 0u);
}

// ==========================================================================
// B) CONCURRENCY / CONTENTION
// ==========================================================================

TEST_F(LoggerAsyncTest, MultiThreadStress)
{
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t]()
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                Log::Info("thread {} msg {}", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kThreads * kPerThread));
}

TEST_F(LoggerAsyncTest, ConcurrentFlush_NoDeadlock)
{
    constexpr int kFlushThreads = 4;
    constexpr int kLogCount = 500;

    std::thread producer([]()
    {
        for (int i = 0; i < kLogCount; ++i)
        {
            Log::Info("concurrent-flush msg {}", i);
        }
    });

    std::vector<std::thread> flushers;
    flushers.reserve(kFlushThreads);
    for (int t = 0; t < kFlushThreads; ++t)
    {
        flushers.emplace_back([]()
        {
            for (int i = 0; i < 10; ++i)
            {
                Log::Flush();
            }
        });
    }

    producer.join();
    for (auto& th : flushers) th.join();

    Log::Flush();
    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kLogCount));
}

TEST_F(LoggerAsyncTest, LogDuringShutdown_NoCrash)
{
    std::atomic<bool> go{false};

    std::thread logger([&go]()
    {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 500; ++i)
        {
            Log::Info("during-shutdown {}", i);
        }
    });

    go.store(true, std::memory_order_release);
    Log::Shutdown();
    logger.join();

    // Re-initialize so TearDown works.
    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

TEST_F(LoggerAsyncTest, FlushDuringShutdown_NoHang)
{
    std::atomic<bool> go{false};

    std::thread flusher([&go]()
    {
        while (!go.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 20; ++i)
        {
            Log::Flush();
        }
    });

    for (int i = 0; i < 100; ++i)
    {
        Log::Info("pre-shutdown {}", i);
    }

    go.store(true, std::memory_order_release);
    Log::Shutdown();
    flusher.join();

    Log::Config cfg;
    cfg.GlobalMinLevel = LogLevel::Trace;
    Log::Initialize(cfg);
}

// ==========================================================================
// C) RE-ENTRANCY
// ==========================================================================

TEST_F(LoggerAsyncTest, CallbackSinkThatLogs_NoDeadlock)
{
    auto callbackSink = std::make_unique<CallbackSink>();
    // Heap-owned, captured by value: the sink outlives the test body (TearDown's
    // Shutdown still dispatches to it during the final drain), so the callback
    // must not reference this frame's stack.
    auto reentrantCount = std::make_shared<std::atomic<int>>(0);

    callbackSink->RegisterCallback([reentrantCount](const LogMessage&)
    {
        reentrantCount->fetch_add(1, std::memory_order_relaxed);
        Log::Info("re-entrant message");
    });
    Log::AddSink(std::move(callbackSink));

    Log::Info("trigger");
    Log::Flush();

    EXPECT_GE(reentrantCount->load(), 1);

    // Flush again to drain re-entrant messages.
    Log::Flush();
    EXPECT_GE(m_Spy->GetCount(), 2u);
}

TEST_F(LoggerAsyncTest, CallbackSinkThatFlushes_NoDeadlock)
{
    auto callbackSink = std::make_unique<CallbackSink>();
    // Heap-owned, captured by value: same lifetime hazard as
    // CallbackSinkThatLogs_NoDeadlock above.
    auto flushCallCount = std::make_shared<std::atomic<int>>(0);

    callbackSink->RegisterCallback([flushCallCount](const LogMessage&)
    {
        flushCallCount->fetch_add(1, std::memory_order_relaxed);
        // Re-entrancy guard should short-circuit this without deadlock.
        Log::Flush();
    });
    Log::AddSink(std::move(callbackSink));

    Log::Info("trigger flush re-entrancy");
    Log::Flush();

    EXPECT_GE(flushCallCount->load(), 1);
    EXPECT_GE(m_Spy->GetCount(), 1u);
}

// ==========================================================================
// D) PERFORMANCE REGRESSION
// ==========================================================================

TEST_F(LoggerAsyncTest, ThroughputRegression)
{
    constexpr int kCount = 10000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCount; ++i)
    {
        Log::Info("perf arg1={} arg2={} arg3={} arg4={} arg5={}", i, i * 2, "str", 3.14, true);
    }
    auto enqueueEnd = std::chrono::steady_clock::now();

    Log::Flush();
    auto flushEnd = std::chrono::steady_clock::now();

    auto enqueueUs = std::chrono::duration_cast<std::chrono::microseconds>(enqueueEnd - start).count();
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(flushEnd - start).count();

    std::printf("[PERF] Enqueue %d formatted msgs: %lld us (%.2f us/msg), total with Flush: %lld us\n",
                kCount, static_cast<long long>(enqueueUs),
                static_cast<double>(enqueueUs) / kCount,
                static_cast<long long>(totalUs));

    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kCount));
}

TEST_F(LoggerAsyncTest, MultiThreadThroughputRegression)
{
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t]()
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                Log::Info("mt-perf t={} i={}", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto enqueueEnd = std::chrono::steady_clock::now();
    Log::Flush();
    auto flushEnd = std::chrono::steady_clock::now();

    auto enqueueUs = std::chrono::duration_cast<std::chrono::microseconds>(enqueueEnd - start).count();
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(flushEnd - start).count();

    std::printf("[PERF] %d threads x %d msgs: enqueue %lld us, total %lld us\n",
                kThreads, kPerThread,
                static_cast<long long>(enqueueUs),
                static_cast<long long>(totalUs));

    EXPECT_EQ(m_Spy->GetCount(), static_cast<size_t>(kThreads * kPerThread));
}

} // anonymous namespace
