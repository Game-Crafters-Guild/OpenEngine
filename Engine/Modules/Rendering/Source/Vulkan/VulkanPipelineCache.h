#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace GameEngine
{
namespace Rendering
{

// Vulkan-only wrapper around `VkPipelineCache` for disk persistence.
// Distinct from the backend-agnostic `Rendering::PipelineCache` (interning +
// concrete-pipeline cache); name reflects the binding to Vulkan's API type.
class VulkanPipelineCache
{
  public:
    struct Config
    {
        std::string CacheDirectory = "cache/pipelines/";
        std::string CacheFileName = "vulkan_pipeline_cache.bin";
        // False (the default, as DeviceDesc::persistPipelineCache): the cache lives in
        // memory only. Initialize neither creates the directory nor seeds the
        // VkPipelineCache from the file, and Shutdown never writes it.
        bool PersistToDisk = false;
        bool EnableCompression = true;
        bool EnableValidation = true;
        uint32_t MaxCacheSize = 64 * 1024 * 1024;
        uint32_t CacheVersion = 1;
    };

    // Hits are driver-reported only on the graphics path (creation feedback, Vulkan 1.3);
    // mesh and compute creations are always counted as misses.
    struct Statistics
    {
        uint32_t TotalPipelines = 0;
        uint32_t CacheHits = 0;
        uint32_t CacheMisses = 0;
        size_t CacheSize = 0;
        double AverageCreationTime = 0.0;
        double AverageCachedCreationTime = 0.0;
    };

    // Provide explicit overloads instead of aggregate default argument to avoid
    // compiler issues with nested Config default member initializers on some toolchains.
    explicit VulkanPipelineCache(VkDevice device);
    VulkanPipelineCache(VkDevice device, const Config& config);
    ~VulkanPipelineCache();

    static std::string GetPlatformCacheDirectory();

    VulkanPipelineCache(const VulkanPipelineCache&) = delete;
    VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;
    VulkanPipelineCache(VulkanPipelineCache&&) = delete;
    VulkanPipelineCache& operator=(VulkanPipelineCache&&) = delete;

    bool Initialize();
    // saveToDisk defaults true (the normal shutdown persist point). The Q6
    // in-place device rebuild passes false: the teardown drops this cache object
    // and bringup immediately reconstructs one that reloads the same on-disk
    // bytes, so re-persisting the (up to tens-of-MB) blob mid-recovery is pure
    // latency — the slice-2 smoke measured it dominating recovery wall-time.
    // The saved bytes are one vkGetPipelineCacheData snapshot taken here, at device
    // teardown, after IDevice::InvokePerDeviceCacheCleanups has waited for every
    // running build of the pipeline build table. A creation outside that table
    // (IDevice::CreateGraphicsPipelineDirect, the fault-injection pipeline) may overlap
    // the snapshot (the implementation synchronizes the cache; the snapshot merely
    // misses that pipeline) but must have ended before this call destroys the cache:
    // the handle is externally synchronized at vkDestroyPipelineCache.
    void Shutdown(bool saveToDisk = true);

    // The cache is created without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT,
    // so the implementation synchronizes it: any number of threads may call
    // vkCreateGraphicsPipelines / vkCreateComputePipelines / vkGetPipelineCacheData
    // with it at once, and no caller locks around them. Setting that flag would make
    // every such call need external synchronization again.
    VkPipelineCache GetVkPipelineCache() const
    {
        return m_PipelineCache;
    }

    bool SaveCache();
    bool LoadCache();
    void ClearCache();

    const Statistics& GetStatistics() const
    {
        return m_Statistics;
    }
    // Thread-safe: pipelines are created on several threads at once, so the
    // statistics are updated under m_StatisticsMutex.
    void RecordPipelineCreation(double creationTimeMs, bool wasCached);
    bool IsValid() const
    {
        return m_PipelineCache != VK_NULL_HANDLE;
    }

    std::filesystem::path GetCacheFilePath() const;

  private:
    VkDevice m_Device;
    VkPipelineCache m_PipelineCache = VK_NULL_HANDLE;
    Config m_Config;
    Statistics m_Statistics;
    bool m_Initialized = false;

    std::mutex m_StatisticsMutex; // Guards m_Statistics in RecordPipelineCreation

    struct CacheHeader
    {
        uint32_t magic = 0x56504C43;
        uint32_t version = 1;
        uint32_t dataSize = 0;
        uint32_t checksum = 0;
        uint32_t compressed = 0;
    };

    bool CreateCacheDirectory();
    // Single-open read + validation of the on-disk cache file, returning the
    // decompressed VkPipelineCache payload. The cache file is machine-global
    // (GetPlatformCacheDirectory) and written by every process whose device
    // persists its cache (the Editor, the Player), so a read can race another
    // process's save: every size here is validated
    // before it reaches an allocation, and any inconsistency (torn write,
    // truncation, failed seek, checksum/size mismatch) yields nullopt — a
    // cold cache, never a throw out of device bring-up.
    std::optional<std::vector<uint8_t>> LoadCacheData();
    // Returns empty when compression is unavailable or fails (caller then
    // stores uncompressed and marks the header accordingly).
    std::vector<uint8_t> CompressData(const std::vector<uint8_t>& data);
    // Requires the exact decompressed size from the header (no estimates);
    // returns empty on unavailability, corruption, or size disagreement.
    std::vector<uint8_t> DecompressData(const std::vector<uint8_t>& compressedData,
                                        uint32_t expectedSize);
    uint32_t CalculateChecksum(const std::vector<uint8_t>& data);
};

class PipelineCreationTimer
{
  public:
    PipelineCreationTimer(VulkanPipelineCache* cache, bool wasCached);
    ~PipelineCreationTimer();

  private:
    VulkanPipelineCache* m_Cache;
    bool m_WasCached;
    std::chrono::high_resolution_clock::time_point m_StartTime;
};

#define PIPELINE_CREATION_TIMER(cache, wasCached) \
    PipelineCreationTimer timer(cache, wasCached)

} // namespace Rendering
} // namespace GameEngine
