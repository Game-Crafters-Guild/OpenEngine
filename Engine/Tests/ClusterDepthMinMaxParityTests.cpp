// GPU-executed parity test for the per-tile depth min/max reduction
// (cluster_depth_minmax.comp), packaged at 32, 16 and 8 threads per axis: the
// DepthMinMax pass runs the largest a device's workgroup limit allows (a phone's
// WebGPU grants 256 invocations, not 1024), each thread reading a square block of
// the 32x32 tile. Every size must write the same (Zmin, Zmax) per tile: the one
// the CPU finds over the tile's opaque pixels, sky (depth 0) excluded, and
// (UINT_MAX, 0) for a tile that is all sky.

#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "TestDeviceHelper.h"

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// Not multiples of 32, so the right and bottom tiles are partial.
constexpr uint32_t kWidth = 70;
constexpr uint32_t kHeight = 45;
constexpr uint32_t kTileSize = 32;
constexpr uint32_t kTilesX = (kWidth + kTileSize - 1) / kTileSize;
constexpr uint32_t kTilesY = (kHeight + kTileSize - 1) / kTileSize;

// Reverse-Z depth: opaque pixels in (0, 1], sky 0. The bottom-right tile is all sky.
std::vector<float> FixtureDepth()
{
    std::vector<float> depth(kWidth * kHeight);
    uint32_t state = 0x9E3779B9u;
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            state = state * 1664525u + 1013904223u;
            const bool sky = (x >= 2 * kTileSize && y >= kTileSize) || (state >> 28) == 0u;
            depth[y * kWidth + x] = sky ? 0.0f : static_cast<float>((state >> 8) % 1000000u + 1u) / 1000000.0f;
        }
    }
    return depth;
}

// (asuint(Zmin), asuint(Zmax)) per tile, row-major, as the kernel writes it.
std::vector<std::array<uint32_t, 2>> ReferenceTiles(const std::vector<float>& depth)
{
    std::vector<std::array<uint32_t, 2>> tiles(kTilesX * kTilesY, {0xFFFFFFFFu, 0u});
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const float d = depth[y * kWidth + x];
            if (!(d > 0.0f))
                continue;
            std::array<uint32_t, 2>& tile = tiles[(y / kTileSize) * kTilesX + x / kTileSize];
            const uint32_t bits = std::bit_cast<uint32_t>(d);
            tile[0] = std::min(tile[0], bits);
            tile[1] = std::max(tile[1], bits);
        }
    }
    return tiles;
}

// The LocalSize a SPIR-V compute module declares.
std::optional<std::array<uint32_t, 3>> SpirvLocalSize(const std::vector<uint8_t>& bytes)
{
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
    for (size_t at = 5; at < words.size() && (words[at] >> 16) != 0;)
    {
        const uint32_t count = words[at] >> 16;
        if ((words[at] & 0xFFFFu) == 16u && count == 6 && words[at + 2] == 17u)
            return std::array<uint32_t, 3>{words[at + 3], words[at + 4], words[at + 5]};
        at += count;
    }
    return std::nullopt;
}

std::vector<uint8_t> LoadKernel(const char* package)
{
    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg(package, ShaderSourceKind::SpirV, pkg, &err))
        return {};
    const auto itCs = pkg.stageBytes.find("cs");
    return itCs == pkg.stageBytes.end() ? std::vector<uint8_t>{} : itCs->second;
}

// Runs `kernel` over the fixture and reads back the tiles.
std::vector<std::array<uint32_t, 2>> RunKernel(IDevice& device, const std::vector<uint8_t>& kernel,
                                               const std::vector<float>& depth)
{
    DescriptorSetLayoutDesc layout{};
    const DescriptorType types[2] = {DescriptorType::CombinedImageSampler, DescriptorType::StorageBuffer};
    for (uint32_t binding = 0; binding < 2; ++binding)
    {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = types[binding];
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    }
    ComputePipelineDesc pipelineDesc{};
    pipelineDesc.ComputeShader = std::make_shared<const std::vector<uint8_t>>(kernel);
    pipelineDesc.DescriptorSetLayouts.push_back(device.InternDescriptorSetLayout(layout));
    pipelineDesc.DebugName = "ClusterDepthMinMaxParityTest";
    const PipelineHandle pipeline = device.GetOrCreateComputePipeline(device.InternComputePipeline(pipelineDesc));
    EXPECT_TRUE(pipeline.IsValid());
    const SamplerHandle sampler = device.CreateSampler(SamplerDesc::PointClamp("ClusterDepthMinMaxParityTest"));

    TextureDesc td{};
    td.width = kWidth;
    td.height = kHeight;
    td.depth = td.mipLevels = td.sampleCount = td.arrayLayers = 1;
    // The kernel samples depth as a float; a buffer copy into a depth-format image writes
    // nothing on Vulkan (the copy's aspect is colour), so the fixture is a colour image.
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.debugName = "ClusterDepthMinMaxParityTest.Depth";
    const TextureHandle texture = device.CreateTexture(td);

    BufferDesc uploadDesc{};
    uploadDesc.size = depth.size() * sizeof(float);
    uploadDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
    uploadDesc.memoryUsage = BufferMemoryUsage::Upload;
    const BufferHandle upload = device.CreateBuffer(uploadDesc);
    constexpr uint64_t kResultBytes = kTilesX * kTilesY * 2 * sizeof(uint32_t);
    BufferDesc resultDesc{};
    resultDesc.size = kResultBytes;
    resultDesc.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
    resultDesc.memoryUsage = BufferMemoryUsage::Readback;
    const BufferHandle result = device.CreateBuffer(resultDesc);
    std::memcpy(device.MapBuffer(upload), depth.data(), uploadDesc.size);
    device.UnmapBuffer(upload);

    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;
    const DescriptorSetHandle set = device.CreateDescriptorSet(setDesc);
    device.UpdateCombinedImageSamplerBinding(set, 0, texture, sampler);
    device.UpdateStorageBufferBinding(set, 1, result, 0, kResultBytes);

    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->CopyBufferToTextureSubresource(upload, texture, 0, 0, kWidth, kHeight);
    commands->Barrier(
        ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
    commands->FillBuffer(result, 0, kResultBytes, 0xDEADBEEFu);
    commands->Barrier(
        ResourceBarrier::CreateBufferBarrier(result, ResourceState::CopyDest, ResourceState::UnorderedAccess));
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->Dispatch(kTilesX, kTilesY, 1);
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();

    std::vector<std::array<uint32_t, 2>> tiles(kTilesX * kTilesY);
    std::memcpy(tiles.data(), device.MapBuffer(result), kResultBytes);
    device.UnmapBuffer(result);

    device.DestroyDescriptorSet(set);
    device.DestroyBuffer(result);
    device.DestroyBuffer(upload);
    device.DestroyTexture(texture);
    device.DestroySampler(sampler);
    return tiles;
}

} // namespace

TEST(ClusterDepthMinMaxParity, EveryWorkgroupSizeWritesTheSameTiles)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    const std::vector<float> depth = FixtureDepth();
    const std::vector<std::array<uint32_t, 2>> expected = ReferenceTiles(depth);
    ASSERT_EQ(expected.back()[0], 0xFFFFFFFFu) << "the all-sky tile is not in the fixture";
    struct Size
    {
        const char* Package;
        uint32_t Threads;
    };
    for (const Size size : {Size{"Shaders/cluster_depth_minmax.shaderpkg", 32},
                            Size{"Shaders/cluster_depth_minmax_t16.shaderpkg", 16},
                            Size{"Shaders/cluster_depth_minmax_t8.shaderpkg", 8}})
    {
        const std::vector<uint8_t> kernel = LoadKernel(size.Package);
        ASSERT_FALSE(kernel.empty()) << size.Package << " is not staged";
        const std::optional<std::array<uint32_t, 3>> local = SpirvLocalSize(kernel);
        ASSERT_TRUE(local.has_value()) << size.Package;
        EXPECT_EQ(*local, (std::array<uint32_t, 3>{size.Threads, size.Threads, 1u})) << size.Package;
        const std::vector<std::array<uint32_t, 2>> tiles = RunKernel(*device, kernel, depth);
        for (size_t tile = 0; tile < expected.size(); ++tile)
            EXPECT_EQ(tiles[tile], expected[tile]) << size.Package << ", tile " << tile;
    }
}
