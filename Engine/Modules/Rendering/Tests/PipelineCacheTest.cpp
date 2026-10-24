/**
 * @file VulkanPipelineCacheTest.cpp
 * @brief Unit tests for Pipeline Cache functionality
 */

#include <gtest/gtest.h>
#include "VulkanPipelineCache.h"
#include "Logger/LogSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "ScopedPlatformCacheRoot.h"
#include "TestTempDir.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

class VulkanPipelineCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a mock Vulkan device (VK_NULL_HANDLE for testing)
        m_Device = VK_NULL_HANDLE;
        
        // Use a test-specific cache directory, relative to the working directory every test
        // process of this build shares; the process id keeps a concurrent run's cleanup out of it.
        m_Config.CacheDirectory = "test_cache_" + std::to_string(GameEngine::TestUtils::GetProcessIdForTests()) + "/";
        m_Config.CacheFileName = "test_pipeline_cache.bin";
        // The fixture is the file-backed cache; the in-memory test turns this off itself.
        m_Config.PersistToDisk = true;
        
        // Clean up any existing test cache
        CleanupTestCache();
    }

    void TearDown() override {
        CleanupTestCache();
    }

    void CleanupTestCache() {
        try {
            std::filesystem::remove_all(m_Config.CacheDirectory);
        } catch (...) {
            // Ignore cleanup errors
        }
    }

    VkDevice m_Device;
    VulkanPipelineCache::Config m_Config;
};

TEST_F(VulkanPipelineCacheTest, InitializationWithNullDevice) {
    // Test that pipeline cache handles null device gracefully
    VulkanPipelineCache cache(m_Device, m_Config);
    
    // Should not crash, but may not fully initialize without valid device
    // This tests the error handling paths
    bool result = cache.Initialize();
    
    // With null device, initialization should fail gracefully
    EXPECT_FALSE(result);
}

TEST_F(VulkanPipelineCacheTest, ConfigurationSettings) {
    VulkanPipelineCache::Config config;
    config.CacheDirectory = "custom_cache/";
    config.CacheFileName = "custom_cache.bin";
    config.EnableCompression = false;
    config.EnableValidation = false;
    config.MaxCacheSize = 32 * 1024 * 1024; // 32MB
    config.CacheVersion = 2;

    VulkanPipelineCache cache(m_Device, config);
    
    // Verify configuration is stored correctly
    auto cachePath = cache.GetCacheFilePath();
    // `parent_path()` strips the trailing slash, so compare without it.
    EXPECT_EQ(cachePath.parent_path().string(), "custom_cache");
    EXPECT_EQ(cachePath.filename().string(), "custom_cache.bin");
}

TEST_F(VulkanPipelineCacheTest, CacheDirectoryCreation) {
    VulkanPipelineCache cache(m_Device, m_Config);
    
    // Initialize should create the cache directory
    cache.Initialize();
    
    // Verify directory was created
    EXPECT_TRUE(std::filesystem::exists(m_Config.CacheDirectory));
    EXPECT_TRUE(std::filesystem::is_directory(m_Config.CacheDirectory));
}

TEST_F(VulkanPipelineCacheTest, CacheFileOperations) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Test cache file path generation
    auto cachePath = cache.GetCacheFilePath();
    EXPECT_FALSE(cachePath.empty());
    EXPECT_EQ(cachePath.filename().string(), m_Config.CacheFileName);
    
    // Test cache clearing
    cache.ClearCache();
    EXPECT_FALSE(std::filesystem::exists(cachePath));
}

TEST_F(VulkanPipelineCacheTest, StatisticsTracking) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Test initial statistics
    auto stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 0u);
    EXPECT_EQ(stats.CacheHits, 0u);
    EXPECT_EQ(stats.CacheMisses, 0u);
    EXPECT_EQ(stats.CacheSize, 0u);

    // Test recording pipeline creation
    cache.RecordPipelineCreation(5.0, false); // Cache miss
    cache.RecordPipelineCreation(1.0, true);  // Cache hit

    stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 2u);
    EXPECT_EQ(stats.CacheHits, 1u);
    EXPECT_EQ(stats.CacheMisses, 1u);
    EXPECT_DOUBLE_EQ(stats.AverageCreationTime, 5.0);
    EXPECT_DOUBLE_EQ(stats.AverageCachedCreationTime, 1.0);
}

TEST_F(VulkanPipelineCacheTest, PipelineCreationTimer) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Test RAII timer
    {
        PipelineCreationTimer timer(&cache, false);
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    auto stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 1u);
    EXPECT_EQ(stats.CacheMisses, 1u);
    EXPECT_GT(stats.AverageCreationTime, 0.0);
}

TEST_F(VulkanPipelineCacheTest, ValidityChecks) {
    VulkanPipelineCache cache(m_Device, m_Config);
    
    // Before initialization
    EXPECT_FALSE(cache.IsValid());
    
    // After initialization (may still be false with null device)
    cache.Initialize();
    // With null device, cache won't be valid, but this tests the API
}

TEST_F(VulkanPipelineCacheTest, MultipleInstances) {
    // Test that multiple cache instances can coexist
    VulkanPipelineCache::Config config1 = m_Config;
    config1.CacheFileName = "cache1.bin";

    VulkanPipelineCache::Config config2 = m_Config;
    config2.CacheFileName = "cache2.bin";
    
    VulkanPipelineCache cache1(m_Device, config1);
    VulkanPipelineCache cache2(m_Device, config2);
    
    cache1.Initialize();
    cache2.Initialize();
    
    // Both should have different cache files
    EXPECT_NE(cache1.GetCacheFilePath(), cache2.GetCacheFilePath());
}

TEST_F(VulkanPipelineCacheTest, ErrorHandling) {
    // Test with invalid cache directory (read-only or invalid path)
    VulkanPipelineCache::Config invalidConfig = m_Config;
    invalidConfig.CacheDirectory = ""; // Invalid empty directory
    
    VulkanPipelineCache cache(m_Device, invalidConfig);
    
    // Should handle invalid configuration gracefully
    bool result = cache.Initialize();
    EXPECT_FALSE(result);
}

// A cache configured for memory only touches no file system path at all: no directory,
// no read, no write. The null device stops Initialize before vkCreatePipelineCache,
// which is past the point where a directory would have been created.
TEST_F(VulkanPipelineCacheTest, AnInMemoryCacheCreatesNoDirectory) {
    m_Config.PersistToDisk = false;
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    EXPECT_FALSE(std::filesystem::exists(m_Config.CacheDirectory));
}

namespace
{
using GameEngine::TestUtils::ScopedPlatformCacheRoot;
using LogLines = std::vector<std::string>;

// This binary links the Rendering module and its Logger objects directly, so the
// pipeline cache logs through the process's own logger state: a sink installed here
// sees its lines. Shared buffer because the logger owns the sink and the test
// outlives it.
class CapturingSink final : public Logger::LogSink
{
  public:
    explicit CapturingSink(std::shared_ptr<LogLines> lines) : m_Lines(std::move(lines)) {}

    void Write(const Logger::LogMessage& message) override
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Lines->push_back(message.Message);
    }
    void Flush() override {}
    bool ShouldLog(Logger::LogLevel) const override { return true; }
    std::string GetName() const override { return "PipelineCacheTestLogCapture"; }

  private:
    std::mutex m_Mutex;
    std::shared_ptr<LogLines> m_Lines;
};

// Replaces the sink set with a capture for the scope's lifetime (the binary installs
// none of its own), and restores an empty sink set on every exit path.
class ScopedLogCapture
{
  public:
    ScopedLogCapture() : m_Lines(std::make_shared<LogLines>())
    {
        Logger::Log::Config config;
        config.GlobalMinLevel = Logger::LogLevel::Warning;
        Logger::Log::Initialize(config);
        Logger::Log::ClearSinks();
        Logger::Log::AddSink(std::make_unique<CapturingSink>(m_Lines));
    }
    ~ScopedLogCapture()
    {
        Logger::Log::Flush();
        Logger::Log::ClearSinks();
    }
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    // Lines logged so far that contain `needle`; dispatch is asynchronous, so the
    // logger is flushed first.
    size_t CountLinesContaining(const char* needle) const
    {
        Logger::Log::Flush();
        size_t count = 0;
        for (const std::string& line : *m_Lines)
            if (line.find(needle) != std::string::npos)
                ++count;
        return count;
    }

  private:
    std::shared_ptr<LogLines> m_Lines;
};

// Writes a cache file at `path` whose header is structurally valid (magic, version 1,
// payload size, uncompressed) but whose checksum does not match the payload, so a
// reader that opens it logs the checksum warning and ignores it.
void WriteCacheFileWithBadChecksum(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    constexpr uint32_t kMagic = 0x56504C43;
    constexpr uint32_t kVersion = 1;
    constexpr uint32_t kPayloadSize = 64;
    constexpr uint32_t kWrongChecksum = 0xFFFFFFFFu;
    constexpr uint32_t kUncompressed = 0;
    const uint32_t header[5] = {kMagic, kVersion, kPayloadSize, kWrongChecksum, kUncompressed};
    const std::vector<char> payload(kPayloadSize, 'x');
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

std::unique_ptr<IDevice> CreateHeadlessDevice(bool persistPipelineCache)
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = false;
    desc.enableDynamicRendering = true;
    desc.persistPipelineCache = persistPipelineCache;
    std::unique_ptr<IDevice> device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}
} // namespace

// A device persists its pipeline cache only when its DeviceDesc asks for it: by default a
// device's whole life leaves no cache file behind, and with persistPipelineCache the file
// appears at destruction.
TEST_F(VulkanPipelineCacheTest, ADeviceWritesItsCacheFileOnlyWhenAskedToPersist) {
    GameEngine::TestUtils::ScopedTempDir root(
        GameEngine::TestUtils::MakeUniqueTempDirectory("pipeline_cache_root"));
    ScopedPlatformCacheRoot redirect(root.Path());
    const std::filesystem::path file =
        std::filesystem::path(VulkanPipelineCache::GetPlatformCacheDirectory()) / "vulkan_pipeline_cache.bin";
    ASSERT_NE(file.string().find(root.Path().string()), std::string::npos)
        << "the platform cache directory did not follow the redirect: " << file;

    {
        std::unique_ptr<IDevice> device = CreateHeadlessDevice(/*persistPipelineCache=*/false);
        if (!device)
            GTEST_SKIP() << "no Vulkan device";
    }
    EXPECT_FALSE(std::filesystem::exists(file)) << file;

    {
        std::unique_ptr<IDevice> device = CreateHeadlessDevice(/*persistPipelineCache=*/true);
        ASSERT_NE(device, nullptr);
    }
    EXPECT_TRUE(std::filesystem::exists(file)) << file;
}

// The other half of the contract: an in-memory device never opens the cache file. A file
// with a bad checksum is planted where the platform cache lives; the device that keeps its
// cache in memory logs nothing about it, and the device that persists reads it, logs the
// checksum warning and ignores it. The second device also proves the capture sees the
// warning at all, so the first count's zero is a measurement and not a silent sink.
TEST_F(VulkanPipelineCacheTest, AnInMemoryDeviceReadsNoCacheFile) {
    GameEngine::TestUtils::ScopedTempDir root(
        GameEngine::TestUtils::MakeUniqueTempDirectory("pipeline_cache_root"));
    ScopedPlatformCacheRoot redirect(root.Path());
    const std::filesystem::path file =
        std::filesystem::path(VulkanPipelineCache::GetPlatformCacheDirectory()) / "vulkan_pipeline_cache.bin";
    ASSERT_NE(file.string().find(root.Path().string()), std::string::npos)
        << "the platform cache directory did not follow the redirect: " << file;
    WriteCacheFileWithBadChecksum(file);
    ASSERT_TRUE(std::filesystem::exists(file)) << file;
    constexpr const char* kChecksumWarning = "PipelineCache: checksum mismatch";

    ScopedLogCapture capture;
    {
        std::unique_ptr<IDevice> device = CreateHeadlessDevice(/*persistPipelineCache=*/false);
        if (!device)
            GTEST_SKIP() << "no Vulkan device";
    }
    EXPECT_EQ(capture.CountLinesContaining(kChecksumWarning), 0u)
        << "an in-memory device opened the on-disk cache";

    {
        std::unique_ptr<IDevice> device = CreateHeadlessDevice(/*persistPipelineCache=*/true);
        ASSERT_NE(device, nullptr);
    }
    EXPECT_EQ(capture.CountLinesContaining(kChecksumWarning), 1u)
        << "the persisting device did not read the planted file, so the capture proves nothing";
}

// Performance test for cache operations
TEST_F(VulkanPipelineCacheTest, CachePerformance) {
    VulkanPipelineCache cache(m_Device, m_Config);
    cache.Initialize();
    
    // Measure statistics update performance
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 1000; ++i) {
        cache.RecordPipelineCreation(1.0, i % 2 == 0);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // Statistics updates should be very fast (< 1ms for 1000 operations)
    EXPECT_LT(duration.count(), 1000);
    
    auto stats = cache.GetStatistics();
    EXPECT_EQ(stats.TotalPipelines, 1000u);
    EXPECT_EQ(stats.CacheHits, 500u);
    EXPECT_EQ(stats.CacheMisses, 500u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
