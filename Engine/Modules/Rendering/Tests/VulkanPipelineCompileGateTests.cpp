// VulkanPipelineCompileGate: pipeline creations run in parallel, except two whose
// stages compile to the same code, which run one after the other so the second is
// served by the pipeline cache instead of compiling the stages again.
#include "Source/Vulkan/VulkanPipelineCompileGate.h"

#include "Rendering/Core/Device.h"
#include "Source/Vulkan/VulkanDevice.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

constexpr auto kOverlapTimeout = std::chrono::seconds(5);
constexpr auto kBlockedProbe = std::chrono::milliseconds(100);

// Headless device under the validation layer; null when either is unavailable.
std::unique_ptr<IDevice> CreateValidatedHeadlessDevice()
{
#if defined(_WIN32)
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableDynamicRendering = true;
    desc.enableDebugLayer = true;
    std::unique_ptr<IDevice> device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}

GraphicsPipelineId InternStages(IDevice& device, const char* vertex, const char* fragment)
{
    GraphicsPipelineDesc desc{};
    desc.DebugName = std::string(vertex) + "+" + fragment;
    desc.VertexShader = std::make_shared<const std::vector<uint8_t>>(Tests::ReadSpirvBytes(vertex));
    desc.PixelShader = std::make_shared<const std::vector<uint8_t>>(Tests::ReadSpirvBytes(fragment));
    if (desc.VertexShader->empty() || desc.PixelShader->empty())
        return {};
    desc.ColorBlend.attachments.resize(1);
    return device.InternGraphicsPipeline(desc);
}

const VulkanPipelineCache::Statistics& CacheStatistics(IDevice& device)
{
    return static_cast<VulkanDevice&>(device).GetVkDiskPipelineCacheStatistics();
}

} // namespace

TEST(VulkanPipelineCompileGate, CreationsWithDifferentStageKeysRunAtOnce)
{
    VulkanPipelineCompileGate gate;
    std::atomic<int> inside{0};
    auto create = [&](uint64_t key)
    {
        const VulkanPipelineCompileGate::Hold hold = gate.Enter(key);
        ++inside;
        const auto deadline = std::chrono::steady_clock::now() + kOverlapTimeout;
        while (inside.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return inside.load() == 2;
    };
    std::future<bool> first = std::async(std::launch::async, create, 1u);
    std::future<bool> second = std::async(std::launch::async, create, 2u);
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
}

TEST(VulkanPipelineCompileGate, CreationsWithTheSameStageKeyRunOneAfterTheOther)
{
    VulkanPipelineCompileGate gate;
    std::atomic<bool> secondInside{false};
    std::future<void> second;
    {
        const VulkanPipelineCompileGate::Hold first = gate.Enter(7u);
        second = std::async(std::launch::async,
                            [&]
                            {
                                const VulkanPipelineCompileGate::Hold hold = gate.Enter(7u);
                                secondInside = true;
                            });
        EXPECT_EQ(second.wait_for(kBlockedProbe), std::future_status::timeout);
        EXPECT_FALSE(secondInside.load());
    }
    EXPECT_EQ(second.wait_for(kOverlapTimeout), std::future_status::ready);
    EXPECT_TRUE(secondInside.load());
}

TEST(VulkanPipelineCompileGate, StageKeyCoversEveryStageBoundaryAndTheSpecializationData)
{
    const std::vector<uint8_t> a = {1, 2, 3, 4};
    const std::vector<uint8_t> b = {5, 6, 7, 8};
    const std::vector<uint8_t> ab = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint8_t> bc = {5, 6, 7, 8, 9, 10, 11, 12};
    const std::vector<uint8_t> c = {9, 10, 11, 12};
    const std::vector<uint8_t> aCopy = a;
    const std::vector<uint8_t> bCopy = b;

    const std::span<const uint8_t> stages[] = {a, b};
    const std::span<const uint8_t> sameStages[] = {aCopy, bCopy};
    // {AB, C} and {A, BC} carry the same bytes in the same order.
    const std::span<const uint8_t> joinedFirst[] = {ab, c};
    const std::span<const uint8_t> joinedSecond[] = {a, bc};
    EXPECT_EQ(VulkanPipelineCompileGate::StageKey(stages, nullptr),
              VulkanPipelineCompileGate::StageKey(sameStages, nullptr));
    EXPECT_NE(VulkanPipelineCompileGate::StageKey(joinedFirst, nullptr),
              VulkanPipelineCompileGate::StageKey(joinedSecond, nullptr));

    const VkSpecializationMapEntry entry{0, 0, sizeof(uint32_t)};
    const uint32_t one = 1;
    const uint32_t two = 2;
    const VkSpecializationInfo specOne{1, &entry, sizeof(one), &one};
    const VkSpecializationInfo specTwo{1, &entry, sizeof(two), &two};
    EXPECT_EQ(VulkanPipelineCompileGate::StageKey(stages, &specOne),
              VulkanPipelineCompileGate::StageKey(sameStages, &specOne));
    EXPECT_NE(VulkanPipelineCompileGate::StageKey(stages, &specOne),
              VulkanPipelineCompileGate::StageKey(stages, &specTwo));
}

// N pipelines with distinct stages and K duplicates of one more pipeline, all created at
// once from N + K threads on one device: the duplicates compile once and are served by the
// pipeline cache K - 1 times, the distinct ones each compile, and the validation layer reports
// no message. (The layer cannot flag concurrent use of an internally synchronized cache; it
// does reject a cache created externally synchronized, through a feature check.)
TEST(VulkanPipelineCompileGate, ConcurrentDuplicatesCompileOnceAndDistinctPipelinesEachCompile)
{
    std::unique_ptr<IDevice> device = CreateValidatedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->GetValidationStats().Enabled)
        GTEST_SKIP() << "Vulkan validation layer not active";
    Tests::InstallTestShaderResolver();

    const GraphicsPipelineId duplicated = InternStages(*device, "triangle.vert.spv", "triangle.frag.spv");
    const GraphicsPipelineId distinct[] = {
        InternStages(*device, "triangle.vert.spv", "solidcolor.frag.spv"),
        InternStages(*device, "fullscreen_noinput.vert.spv", "solidcolor.frag.spv"),
    };
    ASSERT_TRUE(duplicated.IsValid());
    for (const GraphicsPipelineId id : distinct)
        ASSERT_TRUE(id.IsValid());
    PipelineFormatKey formats{};
    formats.ColorCount = 1;
    formats.ColorFormats[0] = TextureFormat::BGRA8_UNORM;
    formats.RasterizationSamples = 1;

    constexpr size_t kDuplicates = 6;
    std::vector<GraphicsPipelineId> requests(kDuplicates, duplicated);
    requests.insert(requests.end(), std::begin(distinct), std::end(distinct));
    std::vector<PipelineHandle> created(requests.size());
    const VulkanPipelineCache::Statistics before = CacheStatistics(*device);
    {
        std::latch start(static_cast<std::ptrdiff_t>(requests.size()));
        std::vector<std::thread> threads;
        for (size_t i = 0; i < requests.size(); ++i)
            threads.emplace_back(
                [&, i]
                {
                    start.arrive_and_wait();
                    created[i] = device->CreateGraphicsPipelineDirect(requests[i], formats);
                });
        for (std::thread& thread : threads)
            thread.join();
    }
    const VulkanPipelineCache::Statistics after = CacheStatistics(*device);
    for (const PipelineHandle handle : created)
        EXPECT_TRUE(handle.IsValid());

    // Verify the instrument: a driver that never reports a pipeline-cache hit cannot
    // show the duplicates sharing one compile.
    const PipelineHandle again = device->CreateGraphicsPipelineDirect(duplicated, formats);
    EXPECT_TRUE(again.IsValid());
    const uint32_t hitsAfterRepeat = CacheStatistics(*device).CacheHits;
    const uint32_t misses = after.CacheMisses - before.CacheMisses;
    const uint32_t hits = after.CacheHits - before.CacheHits;
    for (const PipelineHandle handle : created)
        device->DestroyPipeline(handle);
    device->DestroyPipeline(again);
    EXPECT_EQ(device->GetValidationStats().ErrorCount, 0u);
    if (hitsAfterRepeat == after.CacheHits)
        GTEST_SKIP() << "The driver reports no pipeline-cache hits (creation feedback)";

    EXPECT_EQ(misses, std::size(distinct) + 1);
    EXPECT_EQ(hits, kDuplicates - 1);
}
