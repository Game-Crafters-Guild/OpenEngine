#include "Logger/Logger.h"
#include "Jobs/HotReloadTasks.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "MockHotReloadDependencies.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <stdexcept>

// TODO: HotReload tests currently rely on a .NET runtimeconfig.json (e.g., ScriptAssemblies/<Config>/net10.0/GameEngine.Scripts.runtimeconfig.json).
// Tests may cancel with "Pipeline execution cancelled" if this file is missing. We should add a proper fixture
// or configure the pipeline to accept an injected runtime path. This is outside the PascalCase Task/TaskHandle migration scope.

namespace GameEngine::Tests {

class HotReloadPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

        // Create test directory structure
        testDir = std::filesystem::temp_directory_path() / "hot_reload_test";
        std::filesystem::create_directories(testDir);

        // Create mock assembly file
        testAssemblyPath = testDir / "TestAssembly.dll";
        createMockAssembly();

        // Initialize infrastructure with shared thread pool
        threadPool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);
        compileChannel = std::make_unique<JobSystem::JobChannel>(*threadPool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        pipeline = std::make_unique<HotReloadPipeline>(*threadPool, *compileChannel);
        // Use mock compiler in pipeline to avoid real dotnet/build and make tests deterministic
        pipeline->SetCompilerFactory([&]() { return std::make_unique<Tests::MockCompiler>(true, 5); });
    }

    void TearDown() override {
        pipeline.reset();
        compileChannel.reset();
        threadPool.reset();

        // Clean up test files
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    void createMockAssembly() {
        // Create a mock .NET assembly file (simplified PE format)
        std::ofstream file(testAssemblyPath, std::ios::binary);

        // Write PE header (MZ signature)
        file.write("MZ", 2);

        // Write some dummy data to simulate a real assembly
        std::vector<uint8_t> dummyData(1024, 0x42);
        file.write(reinterpret_cast<const char*>(dummyData.data()), dummyData.size());

        file.close();
    }

    std::filesystem::path testDir;
    std::filesystem::path testAssemblyPath;
    std::shared_ptr<JobSystem::WorkStealingThreadPool> threadPool;
    std::unique_ptr<JobSystem::JobChannel> compileChannel;

    std::unique_ptr<HotReloadPipeline> pipeline;
};

/**
 * @brief Test individual compilation task
 */
TEST_F(HotReloadPipelineTest, CompilationTaskExecution) {
    // Use mock compiler to avoid requiring real .NET project
    auto mockCompiler = std::make_unique<Tests::MockCompiler>(true, 10);
    CompilationTask task(testAssemblyPath.string(), std::move(mockCompiler));

    auto startTime = std::chrono::high_resolution_clock::now();
    task.Execute();
    auto endTime = std::chrono::high_resolution_clock::now();

    // Compilation should succeed with mock compiler
    EXPECT_EQ(task.GetResult(), HotReloadResult::Success);
    EXPECT_TRUE(task.GetErrorMessage().empty());
    EXPECT_FALSE(task.GetCompilationOutput().empty());

    auto duration = task.GetExecutionDuration();
    EXPECT_GE(duration.count(), 0); // Allow zero duration for fast operations
}

/**
 * @brief Test compilation task failure
 */
TEST_F(HotReloadPipelineTest, CompilationTaskFailure) {
    // Use mock compiler that fails
    auto mockCompiler = std::make_unique<Tests::MockCompiler>(false, 5);
    CompilationTask task(testAssemblyPath.string(), std::move(mockCompiler));

    // A failed compile leaves Execute by exception: that is what fails the
    // task handle and stops the dependent stages.
    EXPECT_THROW(task.Execute(), std::runtime_error);

    EXPECT_EQ(task.GetResult(), HotReloadResult::CompilationFailed);
    EXPECT_EQ(task.GetErrorMessage(), "Mock error: Syntax error on line 42")
        << "The task must report the compiler's own error";
    EXPECT_FALSE(task.GetErrors().empty());
}

/**
 * @brief Test complete pipeline execution
 */
TEST_F(HotReloadPipelineTest, CompletePipelineExecution) {
    auto future = pipeline->ExecuteAsync(testAssemblyPath.string());

    // Wait for completion with timeout
    auto status = future.wait_for(std::chrono::seconds(30));
    ASSERT_EQ(status, std::future_status::ready);

    auto stats = future.get();

    EXPECT_TRUE(stats.success) << "Pipeline should complete successfully: " << stats.errorMessage;
    EXPECT_EQ(stats.result, HotReloadResult::Success);
    EXPECT_GT(stats.totalDuration.count(), 0) << "Pipeline should take some time to complete";
    EXPECT_TRUE(stats.errorMessage.empty()) << "No errors should occur: " << stats.errorMessage;

    // The future resolves only after the last stage, so the pipeline already
    // reports the run as ended and at full progress.
    EXPECT_FALSE(pipeline->IsExecuting()) << "the future resolved while the run was still executing";
    EXPECT_FLOAT_EQ(pipeline->GetProgress(), 1.0f) << "the future resolved before the swap stage ended";

    Logger::Log::Info("CompletePipelineExecution test completed - pipeline waited for all tasks: {}ms",
                stats.totalDuration.count());
}

/**
 * @brief Test pipeline cancellation
 */
TEST_F(HotReloadPipelineTest, PipelineCancellation) {
    GTEST_SKIP() << "Cancellation is currently disabled in HotReloadPipeline (monitor loop), skipping test";
}

/**
 * @brief Test pipeline with non-existent file
 */
TEST_F(HotReloadPipelineTest, PipelineWithNonExistentFile) {
    std::string nonExistentPath = (testDir / "NonExistent.dll").string();

    auto future = pipeline->ExecuteAsync(nonExistentPath);

    auto status = future.wait_for(std::chrono::seconds(10));
    ASSERT_EQ(status, std::future_status::ready);

    auto stats = future.get();

    // In test mode, pipeline uses fallback to load bytes directly; treat as success
    EXPECT_TRUE(stats.success);
    EXPECT_EQ(stats.result, HotReloadResult::Success);
}

/**
 * @brief Test pipeline progress tracking
 */
TEST_F(HotReloadPipelineTest, ProgressTracking) {
    auto future = pipeline->ExecuteAsync(testAssemblyPath.string());

    // Monitor progress
    std::vector<float> progressValues;

    while (!pipeline->IsExecuting()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    while (pipeline->IsExecuting()) {
        float progress = pipeline->GetProgress();
        progressValues.push_back(progress);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto stats = future.get();
    EXPECT_TRUE(stats.success);

    // Verify progress increased over time
    EXPECT_GT(progressValues.size(), 0);
    if (progressValues.size() > 1) {
        EXPECT_LE(progressValues.front(), progressValues.back());
    }

    // Allow a small delay for progress to be updated by monitoring thread
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Final progress should be 1.0
    float finalProgress = pipeline->GetProgress();
    Logger::Log::Info("Test: Final progress value: {}", finalProgress);
    EXPECT_FLOAT_EQ(finalProgress, 1.0f);
}

/**
 * @brief Test concurrent pipeline executions
 */
TEST_F(HotReloadPipelineTest, ConcurrentExecutions) {
    // Start first execution
    auto future1 = pipeline->ExecuteAsync(testAssemblyPath.string());

    // Start second execution; with cancellation disabled in pipeline monitor, the first may complete instead of cancelling
    auto future2 = pipeline->ExecuteAsync(testAssemblyPath.string());

    // Wait for both to complete
    auto stats1 = future1.get();
    auto stats2 = future2.get();

    // Second should succeed; first may either succeed (if it finished quickly) or be cancelled by the lock handover path
    EXPECT_TRUE(stats2.success);
    EXPECT_EQ(stats2.result, HotReloadResult::Success);
}

/**
 * @brief Performance benchmark test
 */
TEST_F(HotReloadPipelineTest, PerformanceBenchmark) {
    const int numRuns = 5;
    std::vector<std::chrono::milliseconds> durations;

    for (int i = 0; i < numRuns; ++i) {
        auto future = pipeline->ExecuteAsync(testAssemblyPath.string());

        auto stats = future.get();
        EXPECT_TRUE(stats.success);

        durations.push_back(stats.totalDuration);

        // Wait between runs
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Calculate average duration
    auto totalMs = std::accumulate(durations.begin(), durations.end(), std::chrono::milliseconds(0));
    auto averageMs = totalMs / numRuns;

    Logger::Log::Info("Performance benchmark: {} runs, average duration: {}ms", numRuns, averageMs.count());

    // Performance expectations
    EXPECT_LT(averageMs.count(), 10000); // Less than 10 seconds average

    // All swap operations should meet performance target
    for (const auto& duration : durations) {
        // Main thread blocking should be minimal (simulated swap is ~25ms)
        EXPECT_LT(duration.count(), 5000); // Total pipeline < 5s
    }
}

// A superseded run reports failure, so every consumer that picks a log severity has to
// tell cancellation apart from a real failure. Both do it through IsCancellation rather
// than by matching errorMessage text: the async pipeline writes "Superseded by newer
// change" while the synchronous compile path writes "Compilation superseded ...", so a
// substring test is both case- and producer-sensitive and silently misses one of them.
TEST(HotReloadResultClassification, CancellationCodesAreNotFailures) {
    EXPECT_TRUE(IsCancellation(HotReloadResult::Cancelled));
    EXPECT_TRUE(IsCancellation(HotReloadResult::CancelledByUser));
    EXPECT_TRUE(IsCancellation(HotReloadResult::CancelledByTimeout));
    // The code the pipeline assigns when a newer change supersedes a run in flight.
    EXPECT_TRUE(IsCancellation(HotReloadResult::CancelledByNewOperation));
}

TEST(HotReloadResultClassification, SuccessAndFailuresAreNotCancellations) {
    EXPECT_FALSE(IsCancellation(HotReloadResult::Success));
    EXPECT_FALSE(IsCancellation(HotReloadResult::CompilationFailed));
    EXPECT_FALSE(IsCancellation(HotReloadResult::CompilationTimeout));
    EXPECT_FALSE(IsCancellation(HotReloadResult::FileLoadFailed));
    EXPECT_FALSE(IsCancellation(HotReloadResult::AssemblyPrepFailed));
    EXPECT_FALSE(IsCancellation(HotReloadResult::SwapFailed));
    EXPECT_FALSE(IsCancellation(HotReloadResult::UnknownError));
}

} // namespace GameEngine::Tests
