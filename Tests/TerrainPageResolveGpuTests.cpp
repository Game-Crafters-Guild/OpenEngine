// The paged height resolve on the device: cbt_page.glsl dispatched through a probe kernel
// (TerrainPageResolveProbe.comp) over a cache texture, page table and slot fades uploaded from a
// driven PagedField, read back and held to the CPU mirror (PagedHeightSampler) at the same points.
// The CPU oracles prove the GLSL text is the mirror bit for bit; this proves the device runs it
// as written, with the hardware's bilinear filter in the cache tap. Two field shapes: a unified
// tiled terrain's (the DEM's 8193 x 2049 lattice) and an atlas tiled terrain's (16 x 4 tiles of
// 1025 samples, 16385 x 4097). A Function terrain's pages, fetched as the page loader fetches them,
// resolve on the device to the function's height.

#include "TerrainPagedField.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "TerrainECS/TerrainHeightFunction/RevocableTerrainHeightFunction.h"
#include "TerrainECS/PackedPageTable.h"
#include "TerrainECS/TerrainHeightPageDriver.h"
#include "TerrainECS/TerrainPageTableRing.h"
#include "TerrainECS/TerrainService.h"

#include "Components/Terrain/Terrain.h"
#include "Mathematics/Vector2.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageTableEntry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::TerrainECS;
using namespace GameEngine::TerrainECS::Test;
using GameEngine::PageStreaming::PageAddress;

namespace
{

// The hardware's bilinear weights carry 8 fractional bits (the Vulkan subTexelPrecisionBits every
// desktop vendor reports), so a tap may differ from the exact bilinear by up to 2^-8 of the texel
// differences along each axis: the quantum of the comparison.
constexpr float32 kSubTexelQuantum = 1.0f / 256.0f;

struct FieldShape
{
    const char* Name;
    uint32 SamplesX;
    uint32 SamplesZ;
    PageTableBudget Budget = kDenseFieldTable;
};

// The CBTPageField and CBTPageLevel of cbt_page.glsl, std430.
struct GpuPageField
{
    uint32 Level0SamplesX;
    uint32 Level0SamplesZ;
    uint32 LevelCount;
    uint32 SlotsPerRow;
    uint32 FloorLevel;
    uint32 TierCount;
    uint32 DirectoryOffset;
    uint32 Pad;
};

struct GpuPageLevel
{
    uint32 FirstEntry;
    uint32 PagesX;
    uint32 PagesZ;
    uint32 Pad;
};

struct QueryPoint
{
    float32 U;
    float32 V;
    float32 Lambda;
    float32 Unused;
};

std::vector<uint8_t> ReadProbeSpirv()
{
    std::ifstream file(TERRAIN_PAGE_PROBE_SPV, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Every page of the levels from 2 up, a corner of level 1 and a larger corner of level 0, with a
// level-1 page and a level-0 page held back (the fallback walk), then the held level-1 page and a
// child of it arriving 0.1 s apart (a page and its parent both mid-fade).
void DriveToMixedResidency(PagedField& field)
{
    std::vector<PageAddress> wanted;
    for (const PageAddress& page : field.AllPages())
    {
        const bool level1Corner = page.Level == 1 && page.X < 8 && page.Z < 4;
        const bool level0Corner = page.Level == 0 && page.X < 12 && page.Z < 8;
        const bool heldBack = (page.Level == 1 && page.X == 1 && page.Z == 1) ||
                              (page.Level == 0 && page.X == 5 && page.Z == 6);
        if ((page.Level >= 2 || level1Corner || level0Corner) && !heldBack)
            wanted.push_back(page);
    }
    for (int frame = 0; frame < 60; ++frame)
        field.Step(wanted, 1.0f / 60.0f, 1024);
    wanted.push_back(PageAddress{0, 1, 1, 1});
    field.Step(wanted, 0.1f, 1024);
    wanted.push_back(PageAddress{0, 0, 3, 2});
    field.Step(wanted, 0.1f, 1024);
}

// The largest difference between two neighboring texels of any resident slot: the bound's scale.
float32 LargestNeighborDifference(const PagedField& field)
{
    float32 largest = 0.0f;
    const uint32 stride = PageStreaming::kPageStrideSamples;
    for (const PageStreaming::CachedPage& page : field.Cache.Resident())
    {
        const uint32 slot = field.Cache.SlotOf(page);
        for (uint32 r = 0; r < stride; ++r)
            for (uint32 c = 0; c < stride; ++c)
            {
                const auto at = [&](uint32 sx, uint32 sz) {
                    return field.Texels[std::size_t(field.Geometry.TexelZ(slot, sz)) * field.Geometry.CacheDim +
                                        field.Geometry.TexelX(slot, sx)];
                };
                if (c + 1 < stride)
                    largest = std::max(largest, std::fabs(at(c + 1, r) - at(c, r)));
                if (r + 1 < stride)
                    largest = std::max(largest, std::fabs(at(c, r + 1) - at(c, r)));
            }
    }
    return largest;
}

// A grid over the detailed corner (where levels 0 and 1 are resident) and one over the whole
// field, each at every lambda.
std::vector<QueryPoint> QueryPoints(const FieldShape& shape)
{
    std::vector<QueryPoint> points;
    const float32 cornerU = 16.0f * 128.0f / float32(shape.SamplesX - 1);
    const float32 cornerV = 12.0f * 128.0f / float32(shape.SamplesZ - 1);
    for (float32 lambda : {0.0f, 0.5f, 0.8f, 0.9375f, 1.6f, 2.9f, 4.0f, 30.0f})
    {
        for (uint32 i = 0; i < 128; ++i)
            for (uint32 k = 0; k < 64; ++k)
                points.push_back({cornerU * (float32(i) + 0.37f) / 128.0f, cornerV * (float32(k) + 0.61f) / 64.0f, lambda, 0.0f});
        for (uint32 i = 0; i <= 64; ++i)
            for (uint32 k = 0; k <= 32; ++k)
                points.push_back({float32(i) / 64.0f, float32(k) / 32.0f, lambda, 0.0f});
    }
    return points;
}

// The probe kernel on a Vulkan device: the cache upload and the dispatch over a driven field.
class PageResolveProbe : public ::testing::Test
{
protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDynamicRendering = true;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
        {
            m_Device.reset();
            GTEST_SKIP() << "no Vulkan device";
        }
        const std::vector<uint8_t> spirv = ReadProbeSpirv();
        ASSERT_FALSE(spirv.empty()) << "the probe kernel is missing: " << TERRAIN_PAGE_PROBE_SPV;

        m_Layout.debugName = "TerrainPageResolveProbe_SetLayout";
        for (uint32_t binding = 0; binding < 6u; ++binding)
        {
            DescriptorBinding b{};
            b.binding = binding;
            b.type = binding == 0u ? DescriptorType::CombinedImageSampler : DescriptorType::StorageBuffer;
            b.count = 1u;
            b.shaderStages = kShaderStageCompute;
            m_Layout.bindings.push_back(b);
        }
        ComputePipelineDesc pipeline{};
        pipeline.ComputeShader = std::make_shared<const std::vector<uint8_t>>(spirv);
        pipeline.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(m_Layout));
        pipeline.DebugName = "TerrainPageResolveProbe";
        m_Pipeline = m_Device->GetOrCreateComputePipeline(m_Device->InternComputePipeline(pipeline));
        ASSERT_TRUE(m_Pipeline.IsValid());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        for (BufferHandle buffer : m_Buffers)
            m_Device->DestroyBuffer(buffer);
        if (m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
        if (m_Sampler.IsValid())
            m_Device->DestroySampler(m_Sampler);
        m_Device->Shutdown();
    }

    BufferHandle MakeBuffer(const void* data, size_t bytes, const char* name)
    {
        BufferDesc desc{};
        desc.size = std::max<size_t>(bytes, 16);
        desc.usage = static_cast<uint32_t>(BufferUsage::Storage) | static_cast<uint32_t>(BufferUsage::TransferDst);
        desc.memoryUsage = BufferMemoryUsage::Upload;
        desc.debugName = name;
        BufferHandle buffer = m_Device->CreateBuffer(desc);
        m_Buffers.push_back(buffer);
        if (data != nullptr)
            m_Device->UpdateBuffer(buffer, 0, bytes, data);
        return buffer;
    }

    void UploadCache(const PagedField& field)
    {
        TextureDesc desc{};
        desc.width = field.Geometry.CacheDim;
        desc.height = field.Geometry.CacheDim;
        desc.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) | static_cast<uint32_t>(TextureUsage::TransferDst);
        desc.persistent = true;
        desc.debugName = "TerrainPageResolveProbe.Cache";
        m_Texture = m_Device->CreateTexture(desc);
        ASSERT_TRUE(m_Texture.IsValid());
        const size_t bytes = field.Texels.size() * sizeof(float32);
        BufferHandle staging = m_Device->CreateUploadBuffer(bytes, "TerrainPageResolveProbe.CacheStaging");
        m_Device->UpdateBuffer(staging, 0, bytes, field.Texels.data());
        auto list = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        list->Begin();
        list->CopyBufferToTextureSubresource(staging, m_Texture, 0, 0, field.Geometry.CacheDim, field.Geometry.CacheDim, 0,
                                             size_t(field.Geometry.CacheDim) * sizeof(float32), 1, 0, 0, 0,
                                             ResourceState::Undefined);
        list->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        list->End();
        m_Device->ExecuteCommandLists({list.get()});
        m_Device->WaitForIdle();
        m_Device->DestroyBuffer(staging);
        m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearClamp("TerrainPageResolveProbe.LinearClamp"));
        ASSERT_TRUE(m_Sampler.IsValid());
    }

    // The device resolve at every point.
    std::vector<float32> Dispatch(const PagedField& field, const std::vector<QueryPoint>& points)
    {
        std::vector<GpuPageLevel> levels;
        const std::span<const PageStreaming::PageStoreLevel> tableLevels = field.Table.Levels();
        for (std::size_t level = 0; level < tableLevels.size(); ++level)
            levels.push_back({field.Table.LevelOffsets()[level], tableLevels[level].PagesX, tableLevels[level].PagesZ, 0u});
        const GpuPageField shape{tableLevels.front().SamplesX,
                                 tableLevels.front().SamplesZ,
                                 uint32(tableLevels.size()),
                                 field.Geometry.SlotsPerRow,
                                 field.Table.FloorLevel(),
                                 field.Table.TierCount(),
                                 field.Table.DirectoryOffset(),
                                 0u};
        std::vector<uint8_t> queries(sizeof(GpuPageField) + 16u + points.size() * sizeof(QueryPoint));
        const uint32 count[4] = {uint32(points.size()), 0u, 0u, 0u};
        std::memcpy(queries.data(), &shape, sizeof(shape));
        std::memcpy(queries.data() + sizeof(shape), count, sizeof(count));
        std::memcpy(queries.data() + sizeof(shape) + 16u, points.data(), points.size() * sizeof(QueryPoint));

        const std::span<const uint32> words = field.Table.Words();
        const std::span<const float32> slotFades = field.Table.SlotFades();
        const BufferHandle entries = MakeBuffer(words.data(), words.size_bytes(), "Probe.Entries");
        const BufferHandle fades = MakeBuffer(slotFades.data(), slotFades.size_bytes(), "Probe.Fades");
        const BufferHandle shapes = MakeBuffer(levels.data(), levels.size() * sizeof(GpuPageLevel), "Probe.Levels");
        const BufferHandle query = MakeBuffer(queries.data(), queries.size(), "Probe.Queries");
        const size_t resultBytes = points.size() * sizeof(float32);
        const BufferHandle results = MakeBuffer(nullptr, resultBytes, "Probe.Results");

        DescriptorSetDesc setDesc{};
        setDesc.layout = m_Layout;
        setDesc.transient = true;
        setDesc.debugName = "TerrainPageResolveProbe.Set";
        const DescriptorSetHandle set = m_Device->CreateDescriptorSet(setDesc);
        m_Device->UpdateCombinedImageSamplerBinding(set, 0, m_Texture, m_Sampler);
        m_Device->UpdateStorageBufferBinding(set, 1, entries, 0, words.size_bytes());
        m_Device->UpdateStorageBufferBinding(set, 2, fades, 0, slotFades.size_bytes());
        m_Device->UpdateStorageBufferBinding(set, 3, shapes, 0, levels.size() * sizeof(GpuPageLevel));
        m_Device->UpdateStorageBufferBinding(set, 4, query, 0, queries.size());
        m_Device->UpdateStorageBufferBinding(set, 5, results, 0, resultBytes);

        auto list = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        list->Begin();
        list->SetPipeline(m_Pipeline);
        list->BindDescriptorSet(0, set, m_Pipeline);
        list->Dispatch((uint32(points.size()) + 63u) / 64u, 1, 1);
        list->End();
        m_Device->ExecuteCommandLists({list.get()});
        m_Device->WaitForIdle();

        std::vector<float32> heights(points.size());
        const void* mapped = m_Device->MapBuffer(results);
        if (mapped != nullptr)
            std::memcpy(heights.data(), mapped, resultBytes);
        m_Device->UnmapBuffer(results);
        return heights;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout{};
    PipelineHandle m_Pipeline{};
    TextureHandle m_Texture{};
    SamplerHandle m_Sampler{};
    std::vector<BufferHandle> m_Buffers;
};

class TerrainPageResolveGpu : public PageResolveProbe, public ::testing::WithParamInterface<FieldShape>
{
};

} // namespace

TEST_P(TerrainPageResolveGpu, TheDeviceResolveIsTheMirrorWithinTheSubTexelQuantum)
{
    const FieldShape shape = GetParam();
    PagedField field(640, 0.4f, shape.SamplesX, shape.SamplesZ, shape.Budget);
    DriveToMixedResidency(field);
    ASSERT_NE(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 3, 2})), PageStreaming::kNoResidentSlot);
    ASSERT_LT(field.Cache.FadeOf(PagedField::Page(PageAddress{0, 1, 1, 1})), 1.0f) << "the fade chain is not mid-fade";
    ASSERT_EQ(field.Cache.SlotOf(PagedField::Page(PageAddress{0, 0, 5, 6})), PageStreaming::kNoResidentSlot);

    UploadCache(field);
    const std::vector<QueryPoint> points = QueryPoints(shape);
    const std::vector<float32> device = Dispatch(field, points);
    ASSERT_EQ(device.size(), points.size());

    const float32 bound = 2.0f * kSubTexelQuantum * LargestNeighborDifference(field) + 1.0e-4f;
    const PagedHeightSampler mirror = field.Sampler();
    float32 worst = 0.0f;
    uint32 outside = 0;
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const float32 expected = mirror.Sample(points[i].U, points[i].V, points[i].Lambda);
        const float32 error = std::fabs(device[i] - expected);
        worst = std::max(worst, error);
        if (!(error <= bound) && outside++ < 8)
            ADD_FAILURE() << shape.Name << " uv " << points[i].U << "," << points[i].V << " lambda " << points[i].Lambda
                          << ": device " << device[i] << " mirror " << expected << " (bound " << bound << ")";
    }
    EXPECT_EQ(outside, 0u) << outside << " of " << points.size() << " points outside the bound " << bound;
    std::printf("[ %s ] %zu points, worst |device - mirror| %.6g, bound %.6g\n", shape.Name, points.size(), worst, bound);
}

INSTANTIATE_TEST_SUITE_P(FieldShapes, TerrainPageResolveGpu,
                         ::testing::Values(FieldShape{"UnifiedTiled8193x2049", 8193u, 2049u},
                                           FieldShape{"AtlasTiled16385x4097", 16385u, 4097u},
                                           // Only the top level dense: two tiers of blocks below it (six levels, then two).
                                           FieldShape{"TieredTwoTiers16385x4097", 16385u, 4097u, PageTableBudget{4u, 640u, 640u}}),
                         [](const ::testing::TestParamInfo<FieldShape>& info) { return std::string(info.param.Name); });

namespace
{

// A tilted plane plus a paraboloid in terrain-local meters: no transcendental, so the expected
// height is exact on every platform.
class PlaneParaboloid final : public ITerrainHeightFunction
{
public:
    static constexpr float32 kCurvature = 1.0e-5f; ///< per square meter

    static float32 Height(float32 x, float32 z)
    {
        const float32 dx = x - 300.0f;
        const float32 dz = z - 700.0f;
        return 20.0f + 0.05f * x + 0.03f * z + kCurvature * (dx * dx + dz * dz);
    }

    void HeightRow(float32 z, std::span<const float32> x, float32, std::span<float32> outMeters) const override
    {
        for (std::size_t i = 0; i < x.size(); ++i)
            outMeters[i] = Height(x[i], z);
    }

    TerrainHeightRange HeightRange(float32, float32, float32, float32, float32) const override
    {
        return {0.0f, 256.0f};
    }
};

// A Function terrain of 2 x 2 tiles of 513 samples a meter apart (1025 x 1025 level-0 samples),
// away from the world origin: the function reads terrain-local meters.
TiledTerrainData FunctionTerrain(std::shared_ptr<const RevocableTerrainHeightFunction> function)
{
    TiledTerrainData tiled;
    tiled.Config.Base.Source = Components::TerrainBaseSource::Function;
    tiled.Config.Base.HeightFunction = std::move(function);
    tiled.Config.HeightScale = 256.0f;
    tiled.Config.TileWorldSize = 512.0f;
    tiled.Config.TileConfig.HeightmapWidth = 513;
    tiled.Config.TilesPerAxisX = 2;
    tiled.Config.TilesPerAxisZ = 2;
    tiled.WorldOriginX = -4000.0f;
    tiled.WorldOriginZ = 2500.0f;
    return tiled;
}

// The samples the page loader fetches for page `address` of `source`: the bytes the renderer
// uploads into the cache texture. NaN when the fetch fails, so every probe that reads it fails.
std::vector<float32> FetchedPageSamples(const PageStreaming::PageSource& source, const PageAddress& address)
{
    const PageStreaming::PageFetchOutcome fetched = PageStreaming::FetchPage(source, address);
    if (!fetched.Bytes)
        return std::vector<float32>(PageStreaming::kPageSampleCount, std::numeric_limits<float32>::quiet_NaN());
    const std::span<const float32> samples = PageStreaming::HeightSamples(*fetched.Bytes);
    return std::vector<float32>(samples.begin(), samples.end());
}

class TerrainFunctionPagesGpu : public PageResolveProbe
{
};

} // namespace

TEST_F(TerrainFunctionPagesGpu, TheDrawnHeightIsTheFunctionWithinTheBilinearAndSubTexelError)
{
    const TiledTerrainData tiled =
        FunctionTerrain(std::make_shared<RevocableTerrainHeightFunction>(std::make_unique<PlaneParaboloid>()));
    const float32 band = FunctionBaseHeightBand(tiled.Config);
    PageStreaming::PageSource source;
    source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(MakeGeneratedHeightLattice(tiled));
    const std::span<const PageStreaming::PageStoreLevel> levels = source.Generated->Levels();
    ASSERT_FALSE(levels.empty());

    PagedField field(256, 0.4f, levels.front().SamplesX, levels.front().SamplesZ);
    ASSERT_EQ(field.Table.Levels().size(), levels.size());
    field.Samples = [&source](const PageStreaming::PageStoreLevel&, const PageAddress& address) {
        return FetchedPageSamples(source, address);
    };
    for (int frame = 0; frame < 3; ++frame)
        field.Step(field.AllPages(), 0.5f, 1024);
    for (const PageAddress& page : field.AllPages())
        ASSERT_NE(field.Cache.SlotOf(PagedField::Page(page)), PageStreaming::kNoResidentSlot);

    UploadCache(field);
    const float32 spacing = tiled.Config.TileWorldSize / float32(tiled.Config.TileConfig.HeightmapWidth - 1u);
    const float32 extentX = float32(levels.front().SamplesX - 1u) * spacing;
    const float32 extentZ = float32(levels.front().SamplesZ - 1u) * spacing;
    const Mathematics::Vector2 probes[] = {{100.37f, 200.61f}, {511.5f, 512.25f}, {900.9f, 33.3f}};
    std::vector<QueryPoint> points;
    for (const Mathematics::Vector2& probe : probes)
        points.push_back({probe.x / extentX, probe.y / extentZ, 0.0f, 0.0f});
    const std::vector<float32> device = Dispatch(field, points);
    ASSERT_EQ(device.size(), points.size());

    // The bilinear tap of the paraboloid errs by up to curvature x texel^2 / 2 (a quarter per axis);
    // the hardware's weights by the sub-texel quantum of the steepest texel step.
    const float32 bound = band * 2.0f * kSubTexelQuantum * LargestNeighborDifference(field) +
                          PlaneParaboloid::kCurvature * spacing * spacing * 0.5f + 1.0e-4f;
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const float32 drawn = device[i] * band;
        const float32 expected = PlaneParaboloid::Height(probes[i].x, probes[i].y);
        EXPECT_NEAR(drawn, expected, bound) << "probe (" << probes[i].x << ", " << probes[i].y << ") m";
        std::printf("[ Function ] probe (%.2f, %.2f) m: drawn %.6f m, function %.6f m, bound %.6g m\n", probes[i].x,
                    probes[i].y, drawn, expected, bound);
    }
}

namespace
{
std::vector<uint32> ReadRingWords(IDevice& device, const TerrainPageTableRing::Slot& slot, std::size_t count)
{
    std::vector<uint32> words(count);
    const auto* mapped = static_cast<const uint8*>(device.MapBuffer(slot.Buffer));
    if (mapped == nullptr)
        return {};
    std::memcpy(words.data(), mapped + slot.Offset, count * sizeof(uint32));
    device.UnmapBuffer(slot.Buffer);
    return words;
}

// The words a slot holds when every word of `height`, then of `surface`, is written.
std::vector<uint32> WholeSlotWords(std::span<const uint32> height, std::span<const uint32> surface)
{
    std::vector<uint32> words(height.begin(), height.end());
    words.insert(words.end(), surface.begin(), surface.end());
    return words;
}

// A dense table of a 257 x 257 field with page (0, 0) of level 0 in `slot`, packed as `version`.
PackedPageTable SmallPackedTable(PageTable& table, uint32 slot, uint64 version)
{
    constexpr uint32 kSlots = 4u;
    table.Reset(257, 257, 1u, kSlots, kDenseFieldTable);
    table.SetEntry(PageAddress{0, 0, 0, 0}, PageStreaming::PackPageEntry(slot, 0));
    PackedPageTable packed;
    packed.Update(table, MakePageCacheGeometry(kSlots), version);
    return packed;
}
} // namespace

// The terrain draw and the grass placement bind one page-table ring slot per frame: a second bind of the
// same terrain in the same frame slot gets the same buffer and offset. A slot is rewritten when the
// terrain, its generation or a table version differs, and a larger table grows the ring.
TEST(TerrainPageTableRing, TheDrawAndTheGrassBindOneSlotAndAChangedKeyRewritesIt)
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    std::unique_ptr<IDevice> device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";

    TerrainPageTableRing ring;
    PageTable heightTable;
    PageTable surfaceTable;
    const PackedPageTable height = SmallPackedTable(heightTable, 1u, 5u);
    PackedPageTable surface = SmallPackedTable(surfaceTable, 2u, 2u);
    const uint32 heightWords = static_cast<uint32>(height.Words().size());
    const std::size_t slotWords = heightWords + surface.Words().size();
    const TerrainPageTableRing::Key key{3u, 1u};
    BufferHandle retired;
    const TerrainPageTableRing::Slot draw = ring.Bind(*device, key, height, &surface, 1u, retired);
    ASSERT_TRUE(draw.Buffer.IsValid());
    EXPECT_FALSE(retired.IsValid());
    const TerrainPageTableRing::Slot grass = ring.Bind(*device, key, height, &surface, 1u, retired);
    EXPECT_EQ(grass.Buffer, draw.Buffer) << "the grass binds the buffer the draw binds";
    EXPECT_EQ(grass.Offset, draw.Offset) << "the grass binds the draw's slot";
    EXPECT_EQ(grass.SurfaceBase, heightWords) << "the surface table follows the height table";
    EXPECT_EQ(ReadRingWords(*device, grass, slotWords), WholeSlotWords(height.Words(), surface.Words()));
    const TerrainPageTableRing::Slot other = ring.Bind(*device, key, height, &surface, 2u, retired);
    EXPECT_NE(other.Offset, draw.Offset) << "each frame slot has its own words";

    // A destroy-and-recreate that reuses the terrain index and the table versions: the generation differs.
    PageTable recreatedTable;
    PackedPageTable recreated = SmallPackedTable(recreatedTable, 3u, 5u);
    ASSERT_EQ(recreated.Version(), height.Version());
    TerrainPageTableRing::Key next = key;
    next.Generation = 2u;
    ring.Bind(*device, next, recreated, &surface, 1u, retired);
    EXPECT_EQ(ReadRingWords(*device, draw, slotWords), WholeSlotWords(recreated.Words(), surface.Words()))
        << "a new generation rewrites the slot";
    recreatedTable.SetEntry(PageAddress{0, 0, 0, 0}, PageStreaming::PackPageEntry(0, 0));
    recreated.Update(recreatedTable, MakePageCacheGeometry(4u), 6u);
    ring.Bind(*device, next, recreated, &surface, 1u, retired);
    EXPECT_EQ(ReadRingWords(*device, draw, slotWords), WholeSlotWords(recreated.Words(), surface.Words()))
        << "a height table change rewrites the slot";
    surfaceTable.SetSlotFade(1u, 0.5f);
    surface.Update(surfaceTable, MakePageCacheGeometry(4u), 7u);
    ring.Bind(*device, next, recreated, &surface, 1u, retired);
    EXPECT_EQ(ReadRingWords(*device, draw, slotWords), WholeSlotWords(recreated.Words(), surface.Words()))
        << "a surface table change rewrites the slot";

    // A table past the slot grows the ring; the previous buffer is retired for the caller to destroy.
    PageTable largeTable;
    largeTable.Reset(16385, 16385, 1u, 4u, kDenseFieldTable);
    PackedPageTable large;
    large.Update(largeTable, MakePageCacheGeometry(4u), 8u);
    ASSERT_GT(large.Words().size(), draw.Bytes / sizeof(uint32));
    const TerrainPageTableRing::Slot grown = ring.Bind(*device, next, large, nullptr, 1u, retired);
    ASSERT_TRUE(grown.Buffer.IsValid());
    EXPECT_EQ(retired, draw.Buffer);
    EXPECT_NE(grown.Buffer, draw.Buffer);
    EXPECT_EQ(ReadRingWords(*device, grown, large.Words().size()), WholeSlotWords(large.Words(), {}));
    device->DestroyBuffer(retired);
    ring.Release(device.get());
}

namespace
{

constexpr std::size_t kDifferentSizes = std::numeric_limits<std::size_t>::max();

// The first word where `a` and `b` differ: their size when they are equal, kDifferentSizes when their
// sizes differ.
std::size_t FirstDifferentWord(std::span<const uint32> a, std::span<const uint32> b)
{
    if (a.size() != b.size())
        return kDifferentSizes;
    return static_cast<std::size_t>(std::ranges::mismatch(a, b).in1 - a.begin());
}

// A random page of a one-face `table`.
PageAddress RandomPage(const PageTable& table, std::mt19937& random)
{
    const std::span<const PageStreaming::PageStoreLevel> levels = table.Levels();
    const uint32 level = static_cast<uint32>(random() % levels.size());
    return PageAddress{0, static_cast<uint8>(level), static_cast<uint32>(random() % levels[level].PagesX),
                       static_cast<uint32>(random() % levels[level].PagesZ)};
}

// One version's random changes: entries set (a block taken when a tier's page is new), entries removed
// (a block released when its last page goes), and fades.
void ChangeRandomly(PageTable& table, uint32 slots, std::vector<PageAddress>& set, std::mt19937& random)
{
    const uint32 changes = 1u + random() % 24u;
    for (uint32 change = 0; change < changes; ++change)
    {
        const uint32 kind = random() % 10u;
        if (kind < 5u || set.empty())
        {
            const PageAddress page = RandomPage(table, random);
            table.SetEntry(page, PageStreaming::PackPageEntry(random() % slots, page.Level));
            set.push_back(page);
        }
        else if (kind < 9u)
        {
            const std::size_t at = random() % set.size();
            table.SetEntry(set[at], PageStreaming::kNoPage);
            set[at] = set.back();
            set.pop_back();
        }
        else
        {
            table.SetSlotFade(random() % slots, static_cast<float32>(random() % 16u) / 16.0f);
        }
    }
}

} // namespace

// After random changes, a slot that takes only the changed words equals a whole write of both tables, and its
// reported hash equals a whole pack's. The changes cover entries, blocks taken and released, fades, a reset,
// the run and write caps, and a slot whose version fell out of the history.
TEST(TerrainPageTableRing, ASlotThatTakesOnlyTheChangedWordsHoldsWhatAWholeWriteWould)
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    std::unique_ptr<IDevice> device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        GTEST_SKIP() << "no Vulkan device";

    constexpr uint32 kSlots = 64u;
    constexpr uint32 kSteps = 72u;
    constexpr uint32 kResetAt = 12u;
    // Frame slot 3 and the snapshot skip the steps [kLagFrom, kLagTo): more versions than the history.
    constexpr uint32 kLagFrom = 28u;
    constexpr uint32 kLagTo = 46u;
    constexpr uint32 kManyRunsAt = 56u;
    constexpr uint32 kManyWritesAt = 60u;
    const PageCacheGeometry geometry = MakePageCacheGeometry(kSlots);
    // A tiered height table with small pools: pages wait for blocks, and blocks free and refill.
    const PageTableBudget tiered{4u, 8u, 8u};
    PageTable height;
    height.Reset(16385, 4097, 1u, kSlots, tiered);
    ASSERT_GT(height.TierCount(), 0u);
    const uint32 topLevel = static_cast<uint32>(height.Levels().size() - 1u);
    PageTable surface;
    surface.Reset(8193, 8193, 1u, kSlots, kDenseFieldTable);
    ASSERT_EQ(surface.TierCount(), 0u);
    ASSERT_GE(surface.Levels().front().PagesX, 32u);
    PackedPageTable heightPacked;
    PackedPageTable surfacePacked;
    PackedPageTable snapshot;
    uint64 version = 0;
    heightPacked.Update(height, geometry, ++version);
    surfacePacked.Update(surface, geometry, ++version);

    TerrainPageTableRing ring;
    const TerrainPageTableRing::Key key{3u, 1u};
    std::mt19937 random(0x13A9u);
    std::vector<PageAddress> heightSet;
    std::vector<PageAddress> surfaceSet;
    std::vector<uint32> whole;
    std::vector<uint32> wholeSurface;
    for (uint32 step = 0; step < kSteps; ++step)
    {
        if (step == kResetAt)
        {
            height.Reset(16385, 4097, 1u, kSlots, tiered);
            heightSet.clear();
        }
        ChangeRandomly(height, kSlots, heightSet, random);
        if (step == kManyWritesAt)
            for (uint32 write = 0; write < 1100u; ++write) // past the 1024 writes a table records
                height.SetEntry(PageAddress{0, static_cast<uint8>(topLevel), 0, 0},
                                PageStreaming::PackPageEntry(write % kSlots, topLevel));
        heightPacked.Update(height, geometry, ++version);
        if (step % 3u != 1u || step == kManyRunsAt)
        {
            ChangeRandomly(surface, kSlots, surfaceSet, random);
            if (step == kManyRunsAt)
                for (uint32 page = 0; page < 300u; ++page) // 300 disjoint runs: past the 256 a version records
                    surface.SetEntry(PageAddress{0, 0, (page * 2u) % 32u, (page * 2u) / 32u},
                                     PageStreaming::PackPageEntry(page % kSlots, 0));
            surfacePacked.Update(surface, geometry, ++version);
        }

        PackPageTableWords(height, geometry, whole);
        PackPageTableWords(surface, geometry, wholeSurface);
        ASSERT_EQ(FirstDifferentWord(heightPacked.Words(), whole), whole.size())
            << "step " << step << ": the patched height words differ from a whole pack";
        ASSERT_EQ(FirstDifferentWord(surfacePacked.Words(), wholeSurface), wholeSurface.size())
            << "step " << step << ": the patched surface words differ from a whole pack";

        const bool lagging = step >= kLagFrom && step < kLagTo;
        const uint32 frameSlot = lagging ? step % 3u : step % TerrainPageTableRing::kSlots;
        if (frameSlot == 3u)
        {
            snapshot.CopyFrom(heightPacked);
            ASSERT_EQ(FirstDifferentWord(snapshot.Words(), whole), whole.size())
                << "step " << step << ": the snapshot differs from its source";
        }
        BufferHandle retired;
        const TerrainPageTableRing::Slot slot =
            ring.Bind(*device, key, heightPacked, &surfacePacked, frameSlot, retired);
        ASSERT_TRUE(slot.Buffer.IsValid());
        if (retired.IsValid())
            device->DestroyBuffer(retired);
        const std::vector<uint32> expected = WholeSlotWords(whole, wholeSurface);
        ASSERT_EQ(FirstDifferentWord(ReadRingWords(*device, slot, expected.size()), expected), expected.size())
            << "step " << step << " frame slot " << frameSlot << ": the ring's words differ from a whole write";
        auto list = device->CreateCommandList(IDevice::QueueType::Graphics);
        list->Begin();
        ASSERT_TRUE(ring.RecordSlotCopy(*device, *list, frameSlot)) << "step " << step;
        list->End();
        device->ExecuteCommandLists({list.get()});
        device->WaitForIdle();
        ASSERT_TRUE(ring.SealSlotCopy()) << "step " << step;
        TerrainPageTableRing::SlotReport report;
        ASSERT_TRUE(ring.ReadSlotCopy(*device, report)) << "step " << step;
        EXPECT_FALSE(ring.ReadSlotCopy(*device, report)) << "step " << step << ": a sealed copy is read once";
        EXPECT_EQ(report.Slot, frameSlot);
        EXPECT_EQ(report.HeightVersion, heightPacked.Version());
        EXPECT_EQ(report.HeightWords, whole.size());
        EXPECT_EQ(report.HeightHash, PackedPageTable::HashWords(whole))
            << "step " << step << " frame slot " << frameSlot << ": the copied slot's hash differs from a whole pack's";
    }
    ring.Release(device.get());
}
