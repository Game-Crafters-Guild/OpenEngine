// Executes the shipped placement shader and the production regional upload
// owner on a real Vulkan device. The plan input is a single admitted cell;
// nothing in these tests reimplements placement or the regional compositor.
#include <gtest/gtest.h>
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainGrass/TerrainGrassRenderFeature.h"
#include "TerrainGrass/TerrainGrassPlacementStats.h"
#include "TerrainGrass/GrassPlacementModel.h"
#include "GrassBladeInstanceGPU.h"
#include "Terrain/Heightfield.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainColorMapCompose.h"
#include "TerrainFieldUploader.h"
#include "TerrainECS/TerrainColorMap.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Source/Vulkan/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::TerrainECS;
using namespace GameEngine::TerrainGrass;

class GrassUploadDevice : public VulkanDevice
{
public:
    bool RefuseGrassStaging = false;
    BufferHandle CreateBuffer(const BufferDesc& desc) override
    {
        if (RefuseGrassStaging && desc.debugName
            && std::string_view(desc.debugName) == "Terrain_GrassControl_Upload") return {};
        return VulkanDevice::CreateBuffer(desc);
    }
};

class TerrainGrassRegionGpu : public testing::Test
{
protected:
    std::unique_ptr<GrassUploadDevice> Device;
    std::unique_ptr<TerrainRenderFeature> Feature;
    TerrainFieldUploader Uploader{GrassFieldUploadDesc()};
    TerrainHandle Handle{0,1};
    std::vector<BufferHandle> Buffers;
    std::vector<TextureHandle> Textures;
    PipelineHandle Pipeline{};
    DescriptorSetHandle DescriptorSet{};
    SamplerHandle Sampler{};
    std::filesystem::path Cache;

    void SetUp() override
    {
        Device = std::make_unique<GrassUploadDevice>();
        DeviceDesc desc{};
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableSwapchain = false;
        desc.enableDebugLayer = false;
        if (!Device->Initialize(desc)) GTEST_SKIP() << "Vulkan unavailable";
        Feature = std::make_unique<TerrainRenderFeature>();
        ASSERT_TRUE(Feature->Initialize(Device.get()));
        Cache = std::filesystem::temp_directory_path() / ("GrassRegionGpu-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    void TearDown() override
    {
        if (Device)
        {
            Device->WaitForIdle();
            Feature.reset();
            for (auto buffer : Buffers) Device->DestroyBuffer(buffer);
            for (auto texture : Textures) Device->DestroyTexture(texture);
            if (DescriptorSet.IsValid()) Device->DestroyDescriptorSet(DescriptorSet);
            if (Sampler.IsValid()) Device->DestroySampler(Sampler);
            if (Pipeline.IsValid()) Device->DestroyPipeline(Pipeline);
            Device->Shutdown();
        }
        std::error_code error;
        if (!Cache.empty() && Cache.parent_path() == std::filesystem::temp_directory_path()
            && Cache.filename().string().starts_with("GrassRegionGpu-"))
            std::filesystem::remove_all(Cache, error);
    }
    BufferHandle Buffer(const void* data, size_t size, BufferUsage usage)
    {
        BufferDesc desc{};
        desc.size = size;
        desc.usage = static_cast<uint32>(usage | BufferUsage::TransferSrc | BufferUsage::TransferDst);
        desc.memoryUsage = BufferMemoryUsage::Readback;
        auto buffer = Device->CreateBuffer(desc);
        EXPECT_TRUE(buffer.IsValid());
        Buffers.push_back(buffer);
        if (data) Device->UpdateBuffer(buffer, 0, size, data);
        return buffer;
    }
    std::vector<uint8> ReadMap(TerrainFieldMap kind, uint32 width, uint32 height)
    {
        std::vector<uint8> result(static_cast<size_t>(width) * height * 2);
        auto readback = Device->CreateReadbackBuffer(result.size());
        Buffers.push_back(readback);
        auto texture = Feature->GetFieldTexture(TerrainField::GrassControl, Handle, kind);
        EXPECT_TRUE(texture.IsValid());
        auto command = Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        Feature->FlushPendingUploads(command.get());
        command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource, ResourceState::CopySource));
        command->CopyTextureSubresourceToBuffer(texture, 0, 0, readback, width, height, 0, 0, 0, 0);
        command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopySource, ResourceState::ShaderResource));
        command->End();
        Device->ExecuteCommandLists({command.get()});
        Device->WaitForIdle();
        const void* mapped = Device->MapBuffer(readback);
        EXPECT_NE(mapped, nullptr);
        if (mapped) std::memcpy(result.data(), mapped, result.size());
        Device->UnmapBuffer(readback);
        return result;
    }
    TerrainGrassField Field(uint8 h, uint8 d, uint32 size = 5)
    {
        TerrainGrassField field;
        field.Width = field.Height = size;
        field.Initialized = field.Dirty = true;
        field.Version = 1;
        field.DirtyMaxX = field.DirtyMaxZ = size;
        field.Pixels.resize(size * size * 2);
        for (size_t i = 0; i < field.Pixels.size(); i += 2)
        { field.Pixels[i] = h; field.Pixels[i+1] = d; }
        return field;
    }
    void CompilePlacement()
    {
        ShaderProgramCompileRequest request{};
        const auto modules = std::filesystem::path(GRASS_REGION_MODULES_DIR);
        request.baseDirectory = modules / "TerrainGrass/Shaders/TerrainGrass";
        request.cacheRoot = Cache;
        request.includeDirs = {modules / "TerrainGrass/Shaders", modules / "CBTTerrain/Shaders/CBT",
            modules / "Rendering/Shaders", request.baseDirectory};
        ShaderStageCompileSpec stage{};
        stage.stage = "cs"; stage.sourcePath = "terrain_grass_place.comp";
        request.stages.push_back(stage);
        ShaderProgramCompileResult desktop{};
        std::string error;
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, desktop, &error)) << error;
        ASSERT_FALSE(desktop.stageBytes.at("cs").empty());
        request.stages[0].defines = {"GE_COMPAT_PROFILE"};
        ShaderProgramCompileResult result{};
        ASSERT_TRUE(ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, result, &error)) << error;
        PipelineDesc desc{};
        desc.type = PipelineType::Compute;
        desc.computeShader = result.stageBytes.at("cs");
        MaterialBuilder::BuildPipelineDescFromMeta(result.meta, desc);
        Pipeline = Device->CreatePipeline(desc);
        ASSERT_TRUE(Pipeline.IsValid());
        ASSERT_FALSE(desc.descriptorSetLayouts.empty());
        // All seven buffers, twelve named data maps and the shared sampler.
        ASSERT_EQ(desc.descriptorSetLayouts[0].bindings.size(), 20u);
        DescriptorSetDesc set{};
        set.layout = desc.descriptorSetLayouts[0];
        DescriptorSet = Device->CreateDescriptorSet(set);
        ASSERT_TRUE(DescriptorSet.IsValid());
        SamplerDesc sampler{};
        sampler.minFilter = sampler.magFilter = 1;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = 2;
        Sampler = Device->CreateSampler(sampler);
        ASSERT_TRUE(Sampler.IsValid());
    }
    struct Blade
    {
        float32 X, Z, Y, Height, Width, Yaw;
        uint32 RandomAndCoverage, Shape; // grass_clump.glsl: unorm16 random, unorm8 field coverage
        uint32 Terrain;
        float32 NormalX, NormalZ;
        uint32 Flags;
        uint32 RootColor; // 0 on the compat profile these kernels compile for
    };
    static_assert(sizeof(Blade) == kGrassBladeInstanceBytes);
    struct Placement { std::vector<Blade> Blades; uint32 Accepted = 0; };

    // The height field as the R32 unified height texture the terrain uploads: one texel per sample.
    TextureHandle HeightTexture(const Terrain::HeightfieldData& field)
    {
        TextureDesc desc{};
        desc.width = field.GetWidth();
        desc.height = field.GetHeight();
        desc.format = static_cast<uint32>(TextureFormat::R32_FLOAT);
        desc.usage = static_cast<uint32>(TextureUsage::ShaderResource) | static_cast<uint32>(TextureUsage::TransferDst);
        desc.persistent = true;
        desc.debugName = "GrassRegionGpu.Height";
        const auto texture = Device->CreateTexture(desc);
        EXPECT_TRUE(texture.IsValid());
        Textures.push_back(texture);
        const size_t bytes = field.GetSampleCount() * sizeof(float32);
        const auto staging = Device->CreateUploadBuffer(bytes, "GrassRegionGpu.HeightStaging");
        Buffers.push_back(staging);
        Device->UpdateBuffer(staging, 0, bytes, field.GetRawSamples());
        auto command = Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        command->CopyBufferToTextureSubresource(staging, texture, 0, 0, desc.width, desc.height, 0,
                                                desc.width * sizeof(float32), 1, 0, 0, 0, ResourceState::Undefined);
        command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        command->End();
        Device->ExecuteCommandLists({command.get()});
        Device->WaitForIdle();
        return texture;
    }

    // heightmap: the unified height texture the placement reads; invalid = the flat ground field.
    Placement Place(bool card, bool controls, uint8 height, uint8 density, bool atlas = false, bool resident = true,
                    TextureHandle heightmap = {})
    {
        constexpr uint32 slots = 1024;
        auto field = Field(height, density);
        EXPECT_TRUE(Uploader.UpdateUnified(*Feature, Handle, field));
        const auto texture = Feature->GetFieldTexture(TerrainField::GrassControl, Handle, TerrainFieldMap::Unified);
        auto ground = Field(64, 0);
        const TerrainHandle groundHandle{1, 1};
        EXPECT_TRUE(Uploader.UpdateUnified(*Feature, groundHandle, ground));
        const auto groundTexture = Feature->GetFieldTexture(TerrainField::GrassControl, groundHandle, TerrainFieldMap::Unified);
        Terrain::TerrainGPUParams terrain{};
        terrain.WorldOriginX = terrain.WorldOriginZ = -8;
        terrain.WorldOriginY = 7;
        terrain.HeightScale = 4;
        terrain.WorldSizeX = terrain.WorldSizeZ = 24;
        terrain.GrassEnabled = 1u | (card ? 4u : 0u);
        terrain.GrassBladeHeight = 2;
        terrain.GrassBladeWidth = .1f;
        terrain.GrassTextureSize = 3;
        terrain.GrassMaskThreshold = .5f;
        terrain.GrassClumpSize = 4;
        terrain.GrassClumpTaper = .5f;
        terrain.GrassClumpHeightVariance = .5f;
        terrain.Flags = Terrain::kTerrainFlagHasHeightmap
                      | (controls ? Terrain::kTerrainFlagHasGrassControls : 0u);
        terrain.GrassControlBindless = 0; // real compat device contract
        TerrainGrassRenderFeature::GrassAtlasParamsGPU atlasParams{};
        TileAtlasSlot row{};
        row.Slot = resident ? 0 : kAtlasNoSlot;
        if (atlas)
        {
            terrain.Flags = Terrain::kTerrainFlagAtlasBacked;
            atlasParams.Enabled = 1;
            atlasParams.GrassEnabled = controls ? 1 : 0;
            atlasParams.TileRes = 3; atlasParams.SlotStride = 5;
            atlasParams.AtlasDim = 5; atlasParams.SlotsPerRow = 1;
            atlasParams.TilesPerAxisX = atlasParams.TilesPerAxisZ = 1;
            atlasParams.CoarseDim = 5; atlasParams.RowCount = 1;
            // Both named maps below are valid; their bindless indices remain zero.
        }
        TerrainGrassRenderFeature::GrassPlaceParamsGPU place{};
        place.CellWindow[2] = place.CellWindow[3] = 1;
        place.CameraPos[3] = 1;
        std::vector<uint32> plan(kGrassCellPlanBytes / sizeof(uint32), 0);
        GrassCellPlanGPU cell{slots, std::bit_cast<uint32>(1200.0f), kGrassCellValidBit, 0};
        std::memcpy(plan.data() + kGrassRingHistogramWords, &cell, sizeof(cell));
        GrassIndirectBlockGPU args{}; args.Capacity = slots;
        std::vector<Blade> blank(slots);
        const std::array<BufferHandle,7> buffers = {
            Buffer(&terrain, sizeof(terrain), BufferUsage::Storage),
            Buffer(plan.data(), plan.size()*4, BufferUsage::Storage),
            Buffer(blank.data(), blank.size()*sizeof(Blade), BufferUsage::Storage),
            Buffer(&args, sizeof(args), BufferUsage::Storage),
            Buffer(&atlasParams, sizeof(atlasParams), BufferUsage::Uniform),
            Buffer(&row, sizeof(row), BufferUsage::Storage),
            Buffer(&place, sizeof(place), BufferUsage::Uniform)};
        const size_t sizes[] = {sizeof(terrain), plan.size()*4, blank.size()*sizeof(Blade), sizeof(args), sizeof(atlasParams), sizeof(row), sizeof(place)};
        for (uint32 i=0; i<7; ++i)
        {
            if (i == 4 || i == 6)
                Device->UpdateBufferBinding(DescriptorSet, i, buffers[i], 0, sizes[i]);
            else
                Device->UpdateStorageBufferBinding(DescriptorSet, i, buffers[i], 0, sizes[i]);
        }
        for (uint32 i=7; i<19; ++i) Device->UpdateImageBinding(DescriptorSet, i, texture);
        Device->UpdateImageBinding(DescriptorSet, 7, groundTexture);
        Device->UpdateImageBinding(DescriptorSet, 8, groundTexture);
        Device->UpdateImageBinding(DescriptorSet, 13, heightmap.IsValid() ? heightmap : groundTexture);
        Device->UpdateSamplerBinding(DescriptorSet, 19, Sampler);
        auto command = Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        Feature->FlushPendingUploads(command.get());
        command->SetPipeline(Pipeline);
        command->BindDescriptorSet(0, DescriptorSet, Pipeline);
        command->Dispatch(1,1,1);
        command->End();
        Device->ExecuteCommandLists({command.get()});
        Device->WaitForIdle();
        Placement out; out.Blades.resize(slots);
        if (const void* mapped = Device->MapBuffer(buffers[2])) std::memcpy(out.Blades.data(), mapped, sizes[2]);
        else ADD_FAILURE() << "instance readback unavailable";
        Device->UnmapBuffer(buffers[2]);
        if (const void* mapped = Device->MapBuffer(buffers[3])) std::memcpy(&args, mapped, sizeof(args));
        else ADD_FAILURE() << "counter readback unavailable";
        Device->UnmapBuffer(buffers[3]);
        out.Accepted = args.AcceptedBlades;
        return out;
    }
};

TEST_F(TerrainGrassRegionGpu, RecreatedUnifiedFieldRetriesWholeUploadAfterStagingFailure)
{
    auto field = Field(40, 90);
    ASSERT_TRUE(Uploader.UpdateUnified(*Feature, Handle, field));
    EXPECT_EQ(ReadMap(TerrainFieldMap::Unified,5,5), field.Pixels);
    const auto uploaded = Feature->GetGrassPlacementContentEpoch();
    ASSERT_TRUE(Uploader.UpdateUnified(*Feature, Handle, field));
    EXPECT_EQ(Feature->GetGrassPlacementContentEpoch(), uploaded);
    Feature->ReleaseFieldResources(TerrainField::GrassControl, Handle);
    EXPECT_GT(Feature->GetGrassPlacementContentEpoch(), uploaded);
    ASSERT_FALSE(field.Dirty);
    Device->RefuseGrassStaging = true;
    EXPECT_FALSE(Uploader.UpdateUnified(*Feature, Handle, field));
    EXPECT_TRUE(field.Dirty);
    EXPECT_EQ(field.DirtyMinX, 0u);
    EXPECT_EQ(field.DirtyMaxX, field.Width);
    Device->RefuseGrassStaging = false;
    ASSERT_TRUE(Uploader.UpdateUnified(*Feature, Handle, field));
    EXPECT_EQ(ReadMap(TerrainFieldMap::Unified,5,5), field.Pixels);
}

TEST_F(TerrainGrassRegionGpu, AtlasReadinessWaitsForCoarseAndEveryReassignedSlot)
{
    TiledTerrainData tiled{};
    tiled.GrassRegionsActive = true;
    tiled.GrassCoarseField = Field(50, 80);
    auto tile = std::make_unique<TerrainTileData>();
    tile->LodState = TileLodState::Full; tile->GrassField = Field(20,30);
    tiled.Tiles.emplace(TileCoord{0,0}, std::move(tile));
    const auto geometry = MakeAtlasGeometry(5,1,2,1);
    std::array<TileAtlasSlot,2> rows{}; rows[0].Slot=0; rows[0].Generation=1;
    rows[1].Slot=kAtlasNoSlot;
    Device->RefuseGrassStaging = true;
    EXPECT_FALSE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    Device->RefuseGrassStaging = false;
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    const auto first = ReadMap(TerrainFieldMap::Atlas,7,7);
    EXPECT_EQ(first.front(),20);
    const auto version = Feature->GetGrassPlacementContentEpoch();
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    EXPECT_EQ(Feature->GetGrassPlacementContentEpoch(),version);
    tiled.Tiles.clear();
    tile=std::make_unique<TerrainTileData>();
    tile->LodState=TileLodState::Full; tile->GrassField=Field(150,170);
    tiled.Tiles.emplace(TileCoord{1,0},std::move(tile));
    rows[0].Slot=kAtlasNoSlot; rows[1].Slot=0; rows[1].Generation=2;
    Device->RefuseGrassStaging=true;
    EXPECT_FALSE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    Device->RefuseGrassStaging=false;
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    const auto replacement=ReadMap(TerrainFieldMap::Atlas,7,7);
    EXPECT_EQ(replacement.front(),150);
    EXPECT_EQ(replacement.back(),170);
    // Service replacement between extraction ticks can retain the same atlas
    // row. A fresh CPU field starts Version at one again, but its full dirty
    // extent must win over the old slot's coincident cached version.
    tiled.Tiles.at(TileCoord{1,0})->GrassField=Field(70,90);
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    const auto sameRowReplacement=ReadMap(TerrainFieldMap::Atlas,7,7);
    EXPECT_EQ(sameRowReplacement.front(),70);
    EXPECT_EQ(sameRowReplacement.back(),90);
}

TEST_F(TerrainGrassRegionGpu, AdjacentComposedFieldsKeepEdgesThroughCoarseArrivalAndPartialEdits)
{
    ResolvedModifier modifier{};
    modifier.ModType = ResolvedModifier::Type::Volume;
    modifier.Shape = Components::TerrainModifierShape::Circle;
    modifier.Enabled = true; modifier.Position = {4,0,2};
    modifier.Radius = 2; modifier.Falloff = 1;
    modifier.BoundsMinX = 1; modifier.BoundsMaxX = 7;
    modifier.BoundsMinZ = -1; modifier.BoundsMaxZ = 5;
    ResolvedEffect effect;
    effect.EffectKind = ResolvedEffect::Kind::Grass;
    effect.Grass = {0.2f,0.4f,1.0f}; modifier.Effects.push_back(effect);
    const std::array<ResolvedModifier,1> modifiers{modifier};
    TiledTerrainData tiled{};
    tiled.GrassRegionsActive = true;
    ComposeTerrainGrassField(tiled.GrassCoarseField,9,5,8,4,0,0,modifiers);
    auto left = std::make_unique<TerrainTileData>();
    left->LodState = TileLodState::Full;
    ComposeTerrainGrassField(left->GrassField,5,5,4,4,0,0,modifiers);
    auto right = std::make_unique<TerrainTileData>();
    right->LodState = TileLodState::Coarse;
    // Arrival after modifiers: no per-tile field yet, so upload samples the
    // already-composed global field. It must not momentarily fill with white.
    tiled.Tiles.emplace(TileCoord{0,0},std::move(left));
    tiled.Tiles.emplace(TileCoord{1,0},std::move(right));
    const auto geometry = MakeAtlasGeometry(5,2,2,1);
    std::array<TileAtlasSlot,2> rows{};
    rows[0].Slot=0; rows[1].Slot=1;
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    auto read = ReadMap(TerrainFieldMap::Atlas,14,14);
    auto checkEdges = [&](const auto& pixels) {
        for (uint32 z=0;z<5;++z)
            for (uint32 channel=0;channel<2;++channel)
                EXPECT_EQ(pixels[((z+1)*14+5)*2+channel],pixels[((z+1)*14+8)*2+channel]);
        EXPECT_EQ(pixels[(3*14+8)*2],51);
    };
    checkEdges(read);
    auto& arrived=*tiled.Tiles.at(TileCoord{1,0});
    arrived.LodState=TileLodState::Full;
    ComposeTerrainGrassField(arrived.GrassField,5,5,4,4,4,0,modifiers);
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    read=ReadMap(TerrainFieldMap::Atlas,14,14);
    checkEdges(read);
    // One interior texel edit must leave every other atlas byte intact.
    arrived.GrassField.Pixels[(2*5+2)*2]=3;
    ++arrived.GrassField.Version;
    arrived.GrassField.Dirty=true;
    arrived.GrassField.DirtyMinX=arrived.GrassField.DirtyMinZ=2;
    arrived.GrassField.DirtyMaxX=arrived.GrassField.DirtyMaxZ=3;
    ASSERT_TRUE(Uploader.UpdateAtlas(*Feature,Handle,tiled,geometry,rows));
    const auto changed=ReadMap(TerrainFieldMap::Atlas,14,14);
    read[(3*14+10)*2]=3;
    EXPECT_EQ(changed,read);
}

TEST_F(TerrainGrassRegionGpu, ProductionPlacementKeepsNeutralSitesAndScalesCardsAndRibbons)
{
    CompilePlacement();
    ASSERT_TRUE(Pipeline.IsValid());
    ASSERT_TRUE(DescriptorSet.IsValid());
    ASSERT_TRUE(Sampler.IsValid());
    for (bool cards : {false,true})
    {
        const auto legacy=Place(cards,false,255,255);
        const auto neutral=Place(cards,true,255,255);
        ASSERT_EQ(legacy.Accepted,1024u);
        ASSERT_EQ(neutral.Accepted,legacy.Accepted);
        EXPECT_EQ(std::memcmp(legacy.Blades.data(),neutral.Blades.data(),legacy.Blades.size()*sizeof(Blade)),0);
        const auto reduced=Place(cards,true,128,102);
        ASSERT_GT(reduced.Accepted,200u);
        ASSERT_LT(reduced.Accepted,700u);
        for (size_t i=0;i<reduced.Blades.size();++i)
        {
            const auto& blade=reduced.Blades[i];
            if (blade.Height==0) continue;
            const auto& before=legacy.Blades[i];
            EXPECT_EQ(blade.X,before.X); EXPECT_EQ(blade.Z,before.Z);
            EXPECT_EQ(blade.Y,before.Y);
            EXPECT_NEAR(blade.Y, 7.0f + 4.0f*(64.0f/255.0f), 1e-6f);
            EXPECT_EQ(blade.Yaw,before.Yaw); EXPECT_EQ(blade.RandomAndCoverage & 0xFFFFu,before.RandomAndCoverage & 0xFFFFu);
            EXPECT_NEAR(blade.Height,before.Height*(128.0f/255.0f),1e-6f);
            // A card is one square quad carrying grass artwork, so a region scales the whole card
            // and the art keeps its proportions. A ribbon blade IS the geometry: a shorter blade is
            // simply shorter and its width is unrelated to its height.
            if (cards) EXPECT_NEAR(blade.Width,before.Width*(128.0f/255.0f),1e-6f);
            else       EXPECT_EQ(blade.Width,before.Width);
        }
        EXPECT_EQ(Place(cards,true,0,255).Accepted,0u);
        EXPECT_EQ(Place(cards,true,255,0).Accepted,0u);
        // Real compat availability: named atlas/coarse textures, zero bindless IDs.
        EXPECT_EQ(Place(cards,true,0,255,true,true).Accepted,0u);
        EXPECT_EQ(Place(cards,true,0,255,true,false).Accepted,0u);
        for (bool resident : {false, true})
        {
            const auto atlasLegacy = Place(cards,false,255,255,true,resident);
            const auto atlasNeutral = Place(cards,true,255,255,true,resident);
            ASSERT_EQ(atlasLegacy.Accepted,1024u);
            ASSERT_EQ(atlasNeutral.Accepted,atlasLegacy.Accepted);
            EXPECT_EQ(std::memcmp(atlasLegacy.Blades.data(),atlasNeutral.Blades.data(),
                                  atlasLegacy.Blades.size()*sizeof(Blade)),0);
            const auto atlasReduced = Place(cards,true,128,102,true,resident);
            ASSERT_GT(atlasReduced.Accepted,200u);
            ASSERT_LT(atlasReduced.Accepted,700u);
            for (size_t i=0;i<atlasReduced.Blades.size();++i)
            {
                const auto& blade = atlasReduced.Blades[i];
                if (blade.Height == 0) continue;
                const auto& before = atlasLegacy.Blades[i];
                EXPECT_EQ(blade.X,before.X); EXPECT_EQ(blade.Z,before.Z);
                EXPECT_EQ(blade.Y,before.Y);
                EXPECT_EQ(blade.Yaw,before.Yaw); EXPECT_EQ(blade.RandomAndCoverage & 0xFFFFu,before.RandomAndCoverage & 0xFFFFu);
                EXPECT_NEAR(blade.Height,before.Height*(128.0f/255.0f),1e-6f);
                if (cards) EXPECT_NEAR(blade.Width,before.Width*(128.0f/255.0f),1e-6f);
                else       EXPECT_EQ(blade.Width,before.Width);
            }
        }
    }
}

// A blade stands on the CPU height field: on a 33 x 33 lattice with a one-cell cliff every four
// samples and a slope along z, every placed root sits at HeightfieldData::SampleBilinear's height
// for its XZ, the height the terrain is drawn at.
TEST_F(TerrainGrassRegionGpu, PlacedRootsStandOnTheCpuHeightfield)
{
    CompilePlacement();
    ASSERT_TRUE(Pipeline.IsValid());
    constexpr uint32 kDim = 33;
    Terrain::HeightfieldData field(kDim, kDim);
    for (uint32 z = 0; z < kDim; ++z)
        for (uint32 x = 0; x < kDim; ++x)
            field.SetSample(x, z, 0.15f * static_cast<float32>(x / 4u) + 0.3f * static_cast<float32>(z) / (kDim - 1));
    const auto placement = Place(false, false, 255, 255, false, true, HeightTexture(field));
    ASSERT_GT(placement.Accepted, 0u);
    uint32 checked = 0;
    float32 worst = 0.0f;
    for (const auto& blade : placement.Blades)
    {
        if (blade.Height == 0) continue;
        // Place's terrain: origin (-8, 7, -8), 24 m square, height scale 4.
        const float32 expected = 7.0f + 4.0f * field.SampleBilinear((blade.X + 8.0f) / 24.0f, (blade.Z + 8.0f) / 24.0f);
        worst = std::max(worst, std::abs(blade.Y - expected));
        ++checked;
    }
    EXPECT_EQ(checked, placement.Accepted);
    EXPECT_LT(worst, 1e-2f) << "a placed blade root is off the CPU height field";
}
// ---- The colour map on the device ------------------------------------------------------------------

// A colour-map texel the production texture holds, and what the sampler returns for it: the GPU
// decodes the sRGB rgb before it filters, so a written texel beside an unwritten one filters, in
// linear light, to half its premultiplied colour at half its weight, and the ground then reads the
// half mix. An unwritten texel reads exactly 0, which the apply leaves as the ground.
class TerrainColorMapGpu : public TerrainGrassRegionGpu
{
protected:
    // Samples `texture` at each UV with a linear clamp sampler in a compute stage and returns the
    // filtered RGBA, decoded as the surface reads it.
    std::vector<std::array<float32, 4>> Sample(TextureHandle texture, const std::vector<std::array<float32, 2>>& uvs)
    {
        std::filesystem::create_directories(Cache);
        const auto source = Cache / "color_map_sample.comp";
        std::ofstream(source)
            << "#version 460\n"
               "layout(local_size_x = 1) in;\n"
               "layout(set = 0, binding = 0) uniform texture2D gMap;\n"
               "layout(set = 0, binding = 1) uniform sampler gMapSampler;\n"
               "layout(std430, set = 0, binding = 2) buffer Samples { vec4 values[]; } gSamples;\n"
               "void main() { vec2 uv = gSamples.values[gl_GlobalInvocationID.x].xy;\n"
               "  gSamples.values[gl_GlobalInvocationID.x] = textureLod(sampler2D(gMap, gMapSampler), uv, 0.0); }\n";
        ShaderProgramCompileRequest request{};
        request.baseDirectory = Cache;
        request.cacheRoot = Cache / "cache";
        ShaderStageCompileSpec stage{};
        stage.stage = "cs"; stage.sourcePath = "color_map_sample.comp";
        request.stages.push_back(stage);
        ShaderProgramCompileResult result{};
        std::string error;
        EXPECT_TRUE(ShaderCompileService::CompileProgramToCache(request, ShaderSourceKind::SpirV, result, &error)) << error;
        PipelineDesc desc{};
        desc.type = PipelineType::Compute;
        desc.computeShader = result.stageBytes.at("cs");
        MaterialBuilder::BuildPipelineDescFromMeta(result.meta, desc);
        Pipeline = Device->CreatePipeline(desc);
        EXPECT_TRUE(Pipeline.IsValid());
        DescriptorSetDesc set{};
        set.layout = desc.descriptorSetLayouts.at(0);
        DescriptorSet = Device->CreateDescriptorSet(set);
        SamplerDesc sampler{};
        sampler.minFilter = sampler.magFilter = 1;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = 2;
        Sampler = Device->CreateSampler(sampler);
        std::vector<std::array<float32, 4>> values(uvs.size());
        for (size_t i = 0; i < uvs.size(); ++i)
            values[i] = {uvs[i][0], uvs[i][1], 0.0f, 0.0f};
        const size_t bytes = values.size() * sizeof(values[0]);
        const auto buffer = Buffer(values.data(), bytes, BufferUsage::Storage);
        Device->UpdateImageBinding(DescriptorSet, 0, texture);
        Device->UpdateSamplerBinding(DescriptorSet, 1, Sampler);
        Device->UpdateStorageBufferBinding(DescriptorSet, 2, buffer, 0, bytes);
        auto command = Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        Feature->FlushPendingUploads(command.get());
        command->SetPipeline(Pipeline);
        command->BindDescriptorSet(0, DescriptorSet, Pipeline);
        command->Dispatch(static_cast<uint32>(values.size()), 1, 1);
        command->End();
        Device->ExecuteCommandLists({command.get()});
        Device->WaitForIdle();
        if (const void* mapped = Device->MapBuffer(buffer)) std::memcpy(values.data(), mapped, bytes);
        else ADD_FAILURE() << "sample readback unavailable";
        Device->UnmapBuffer(buffer);
        return values;
    }

    std::vector<uint8> ReadColorAtlas(uint32 dim)
    {
        std::vector<uint8> atlas(static_cast<size_t>(dim) * dim * kTerrainColorMapChannels);
        auto readback = Device->CreateReadbackBuffer(atlas.size());
        Buffers.push_back(readback);
        const auto texture = Feature->GetFieldTexture(TerrainField::Color, Handle, TerrainFieldMap::Atlas);
        EXPECT_TRUE(texture.IsValid());
        auto command = Device->CreateCommandList(IDevice::QueueType::Graphics);
        command->Begin();
        Feature->FlushPendingUploads(command.get());
        command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::ShaderResource, ResourceState::CopySource));
        command->CopyTextureSubresourceToBuffer(texture, 0, 0, readback, dim, dim, 0, 0, 0, 0);
        command->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopySource, ResourceState::ShaderResource));
        command->End();
        Device->ExecuteCommandLists({command.get()});
        Device->WaitForIdle();
        if (const void* mapped = Device->MapBuffer(readback)) std::memcpy(atlas.data(), mapped, atlas.size());
        else ADD_FAILURE() << "atlas readback unavailable";
        Device->UnmapBuffer(readback);
        return atlas;
    }

    static TerrainColorMap UniformMap(uint8 r, uint8 a, uint32 size)
    {
        TerrainColorMap map;
        map.Width = map.Height = size;
        map.Initialized = map.Dirty = true;
        map.Version = 1;
        map.DirtyMaxX = map.DirtyMaxZ = size;
        map.Pixels.resize(static_cast<size_t>(size) * size * kTerrainColorMapChannels, 0);
        for (size_t i = 0; i < map.Pixels.size(); i += kTerrainColorMapChannels)
        {
            map.Pixels[i] = r;
            map.Pixels[i + 3] = a;
        }
        return map;
    }
};

TEST_F(TerrainColorMapGpu, TheMidpointOfAWrittenAndAnUnwrittenTexelIsTheHalfMix)
{
    // 2 x 1: the left texel holds colour (0.6, 0.3, 0.1) at weight 0.8, the right one nothing.
    const float32 color[3] = {0.6f, 0.3f, 0.1f};
    const float32 weight = 0.8f;
    TerrainColorMap map;
    map.Width = 2;
    map.Height = 1;
    map.Pixels = {EncodeLinearToSrgbByte(color[0] * weight), EncodeLinearToSrgbByte(color[1] * weight),
                  EncodeLinearToSrgbByte(color[2] * weight), static_cast<uint8>(std::lround(weight * 255.0f)),
                  0, 0, 0, 0};
    map.Initialized = map.Dirty = true;
    map.Version = 1;
    map.DirtyMaxX = 2;
    map.DirtyMaxZ = 1;
    TerrainFieldUploader uploader{ColorMapUploadDesc()};
    ASSERT_TRUE(uploader.UpdateUnified(*Feature, Handle, map));
    const auto texture = Feature->GetFieldTexture(TerrainField::Color, Handle, TerrainFieldMap::Unified);
    ASSERT_TRUE(texture.IsValid());
    const auto samples = Sample(texture, {{0.25f, 0.5f}, {0.5f, 0.5f}, {0.75f, 0.5f}});
    const float32 storedWeight = static_cast<float32>(std::lround(weight * 255.0f)) / 255.0f;
    // The written texel decodes to the premultiplied colour within its byte's step.
    for (uint32 c = 0; c < 3; ++c)
        EXPECT_NEAR(samples[0][c], color[c] * weight, 4e-3f) << c;
    EXPECT_NEAR(samples[0][3], storedWeight, 1e-4f);
    // The midpoint is half of the written texel in every channel.
    for (uint32 c = 0; c < 4; ++c)
        EXPECT_NEAR(samples[1][c], samples[0][c] * 0.5f, 2e-3f) << c;
    // So the ground reads the half mix: ground x (1 - a/2) + c a/2 = mix(ground, c, a/2).
    const float32 ground[3] = {0.10f, 0.16f, 0.035f};
    for (uint32 c = 0; c < 3; ++c)
    {
        const float32 applied = ground[c] * (1.0f - samples[1][3]) + samples[1][c];
        EXPECT_NEAR(applied, ground[c] + (color[c] - ground[c]) * storedWeight * 0.5f, 3e-3f) << c;
    }
    // The unwritten texel is exactly identity, and finite.
    for (uint32 c = 0; c < 4; ++c)
    {
        EXPECT_TRUE(std::isfinite(samples[2][c]));
        EXPECT_EQ(samples[2][c], 0.0f) << c;
    }
}

// A tile's colour map lands in its atlas slot as the splat does: the tile texels at the slot's
// interior with the apron duplicating its edge, and a tile that has not composed reads the coarse map.
TEST_F(TerrainColorMapGpu, TheAtlasSlotHoldsTheTileMapAndAnUncomposedTileReadsTheCoarseMap)
{
    TiledTerrainData tiled{};
    tiled.ColorMapActive = true;
    tiled.ColorCoarseMap = UniformMap(40, 90, 5);
    auto composed = std::make_unique<TerrainTileData>();
    composed->LodState = TileLodState::Full;
    composed->ColorMap = UniformMap(200, 255, 5);
    auto pending = std::make_unique<TerrainTileData>();
    pending->LodState = TileLodState::Full;
    tiled.Tiles.emplace(TileCoord{0, 0}, std::move(composed));
    tiled.Tiles.emplace(TileCoord{1, 0}, std::move(pending));
    const auto geometry = MakeAtlasGeometry(5, 2, 2, 1);
    std::array<TileAtlasSlot, 2> rows{};
    rows[0].Slot = 0;
    rows[1].Slot = 1;
    TerrainFieldUploader uploader{ColorMapUploadDesc()};
    ASSERT_TRUE(uploader.UpdateAtlas(*Feature, Handle, tiled, geometry, rows));
    const auto atlas = ReadColorAtlas(geometry.AtlasDim);
    const auto at = [&](uint32 x, uint32 y)
    { return &atlas[(static_cast<size_t>(y) * geometry.AtlasDim + x) * kTerrainColorMapChannels]; };
    // Slot 0: the composed tile, apron included.
    EXPECT_EQ(at(0, 0)[0], 200);
    EXPECT_EQ(at(3, 3)[0], 200);
    EXPECT_EQ(at(3, 3)[3], 255);
    // Slot 1, the next along the row: the coarse map under a tile that has not composed.
    EXPECT_EQ(at(geometry.SlotStride + 3, 3)[0], 40);
    EXPECT_EQ(at(geometry.SlotStride + 3, 3)[3], 90);
}

// A tile map on another lattice than the slot's is resampled into the slot in linear light, as the
// sampler filters it: between a full texel and an unwritten one the colour is the encoded half, not
// the half of the encoded bytes, which would be darker.
TEST_F(TerrainColorMapGpu, AResampledTileMapBlendsItsColorInLinearLight)
{
    TiledTerrainData tiled{};
    tiled.ColorMapActive = true;
    tiled.ColorCoarseMap = UniformMap(0, 0, 5);
    auto tile = std::make_unique<TerrainTileData>();
    tile->LodState = TileLodState::Full;
    tile->ColorMap = UniformMap(0, 0, 2);
    for (uint32 z = 0; z < 2; ++z)
    {
        uint8* left = tile->ColorMap.Pixels.data() + static_cast<size_t>(z) * 2 * kTerrainColorMapChannels;
        left[0] = left[1] = left[2] = left[3] = 255;
    }
    tiled.Tiles.emplace(TileCoord{0, 0}, std::move(tile));
    const auto geometry = MakeAtlasGeometry(5, 2, 2, 1);
    std::array<TileAtlasSlot, 2> rows{};
    rows[0].Slot = 0;
    rows[1].Slot = 1;
    TerrainFieldUploader uploader{ColorMapUploadDesc()};
    ASSERT_TRUE(uploader.UpdateAtlas(*Feature, Handle, tiled, geometry, rows));
    const auto atlas = ReadColorAtlas(geometry.AtlasDim);
    // Tile texel (2, 2) of 5 sits half way between the 2 x 2 map's columns.
    const uint8* mid = &atlas[(static_cast<size_t>(3) * geometry.AtlasDim + 3) * kTerrainColorMapChannels];
    EXPECT_EQ(mid[0], EncodeLinearToSrgbByte(0.5f));
    EXPECT_EQ(mid[3], 128) << "the weight is linear and filters as bytes";
}

} // namespace
