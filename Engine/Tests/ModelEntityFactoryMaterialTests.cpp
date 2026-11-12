// Materials in ModelEntityFactory: the first spawn of a model converts and
// registers its embedded materials; a later spawn of the same resident model
// reuses them and costs only the entity creation. A reload of the model drops
// them, so the next spawn registers the reloaded materials. Device-gated: the
// factory registers GPU meshes and materials through RenderServices.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelEntityFactory.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"

#include "EngineLogCapture.h"
#include "ModelAssetFbxTestAccess.h"
#include "TestDeviceHelper.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

using GameEngine::GUID;
using GameEngine::ModelAsset;
using GameEngine::Engine::Renderer::ModelEntityFactory;
using GameEngine::Engine::Renderer::ModelMaterialBridge;
using GameEngine::Engine::Renderer::RenderServices;

constexpr GameEngine::uint32 kPieceSubmeshCount = 4;

GameEngine::Vector<GameEngine::EmbeddedImage> TwoOpaqueTexels()
{
    static constexpr GameEngine::uint8 kTga[] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0,
                                                 32, 8, 255, 255, 255, 255};
    GameEngine::Vector<GameEngine::EmbeddedImage> images(2);
    for (auto& image : images)
    {
        image.MimeType = "image/x-tga";
        image.Data = GameEngine::ByteBuffer::CopyOf(kTga);
    }
    return images;
}

// A building piece: four submeshes of one box each, each with its own material;
// the first two materials sample the two embedded images.
void BuildPiece(ModelAsset& model)
{
    GameEngine::Vector<GameEngine::Mesh> meshes;
    GameEngine::Vector<GameEngine::ImportedMaterialData> materials;
    for (GameEngine::uint32 index = 0; index < kPieceSubmeshCount; ++index)
    {
        GameEngine::Mesh mesh;
        mesh.Name = "Part" + std::to_string(index);
        mesh.Vertices.resize(24);
        for (GameEngine::uint32 vertex = 0; vertex < 24; ++vertex)
            mesh.Vertices[vertex].Position[0] = static_cast<float>(vertex % 2);
        for (GameEngine::uint32 face = 0; face < 6; ++face)
        {
            const GameEngine::uint32 base = face * 4;
            mesh.Indices.insert(mesh.Indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        mesh.MaterialIndex = index;
        meshes.push_back(mesh);

        GameEngine::ImportedMaterialData material{};
        material.Name = "PieceMaterial" + std::to_string(index);
        std::fill_n(material.DiffuseColor, 4, 1.0f);
        if (index < 2)
        {
            material.DiffuseTexture = "__embedded:0";
            material.NormalTexture = "__embedded:1";
        }
        materials.push_back(material);
    }
    GameEngine::ModelAssetFbxTestAccess::DemoteMasks(model, meshes, materials, TwoOpaqueTexels());
    model.SetMeshesForTest(std::move(meshes));
}

std::vector<GameEngine::uint32> ContentRevisions(RenderServices& rs, const GUID& modelGuid)
{
    std::vector<GameEngine::uint32> revisions;
    for (GameEngine::uint32 index = 0; index < kPieceSubmeshCount; ++index)
    {
        const auto* material =
            rs.Materials().Registry().Find(ModelMaterialBridge::DeriveMaterialGuid(modelGuid, index));
        revisions.push_back(material ? material->GetContentRevision() : 0u);
    }
    return revisions;
}

} // namespace

// The second spawn of a resident model registers no material again: each derived
// material keeps its content revision, and the new entities still draw with it.
TEST(ModelEntityFactoryMaterials, ASecondSpawnRegistersNoMaterialAgain)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    const GUID modelGuid = GUID::Generate();
    ModelAsset model(modelGuid, "synthetic://piece.gltf");
    BuildPiece(model);

    const auto first = ModelEntityFactory::CreateFromModel(rs, world, model, modelGuid, "Piece");
    ASSERT_EQ(first.submeshEntities.size(), kPieceSubmeshCount);
    const auto afterFirst = ContentRevisions(rs, modelGuid);
    for (const GameEngine::uint32 revision : afterFirst)
        ASSERT_NE(revision, 0u) << "the first spawn registers every material";

    const auto second = ModelEntityFactory::CreateFromModel(rs, world, model, modelGuid, "Piece");
    ASSERT_EQ(second.submeshEntities.size(), kPieceSubmeshCount);
    EXPECT_EQ(ContentRevisions(rs, modelGuid), afterFirst);

    for (GameEngine::uint32 index = 0; index < kPieceSubmeshCount; ++index)
    {
        const auto* renderer = world.GetComponent<GameEngine::Components::MeshRenderer>(second.submeshEntities[index]);
        ASSERT_NE(renderer, nullptr);
        EXPECT_EQ(renderer->materialAssetGuid.ToGuid(), ModelMaterialBridge::DeriveMaterialGuid(modelGuid, index));
    }

    device->Shutdown();
}

// A reload of the model drops its kept materials: the next spawn registers them
// again from the reloaded model.
TEST(ModelEntityFactoryMaterials, ASpawnAfterAReloadRegistersTheMaterialsAgain)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    const GUID modelGuid = GUID::Generate();
    ModelAsset model(modelGuid, "synthetic://piece.gltf");
    BuildPiece(model);

    (void)ModelEntityFactory::CreateFromModel(rs, world, model, modelGuid, "Piece");
    const auto beforeReload = ContentRevisions(rs, modelGuid);

    (void)rs.GetMeshGPURegistry().ReloadModelMeshes(modelGuid, model);
    (void)ModelEntityFactory::CreateFromModel(rs, world, model, modelGuid, "Piece");
    const auto afterReload = ContentRevisions(rs, modelGuid);
    for (GameEngine::uint32 index = 0; index < kPieceSubmeshCount; ++index)
        EXPECT_GT(afterReload[index], beforeReload[index]) << "material " << index;

    device->Shutdown();
}

// Only the first spawn of a model writes its [ModelLoad] line at Info; a restore
// of many pieces of one model writes one Info line, not one per piece.
TEST(ModelEntityFactoryMaterials, OnlyTheFirstSpawnWritesTheModelLoadInfoLine)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    const GUID modelGuid = GUID::Generate();
    ModelAsset model(modelGuid, "synthetic://piece.gltf");
    BuildPiece(model);

    std::vector<std::string> lines;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Info);
        for (int spawn = 0; spawn < 3; ++spawn)
            (void)ModelEntityFactory::CreateFromModel(rs, world, model, modelGuid, "Piece");
        Logger::Log::Flush();
    }
    const auto modelLoadLines = std::count_if(lines.begin(), lines.end(), [](const std::string& line)
                                              { return line.find("[ModelLoad] 'Piece'") != std::string::npos; });
    EXPECT_EQ(modelLoadLines, 1);

    device->Shutdown();
}

// Timing of 1000 spawns of one resident model. Run by name in Release:
// --gtest_also_run_disabled_tests --gtest_filter=*DISABLED_SpawnTimingOfAResidentModel
TEST(ModelEntityFactoryMaterials, DISABLED_SpawnTimingOfAResidentModel)
{
    constexpr size_t kSpawnCount = 1000;
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));
    GameEngine::ECS::World world;

    const GUID modelGuid = GUID::Generate();
    ModelAsset model(modelGuid, "synthetic://piece.gltf");
    BuildPiece(model);

    std::vector<Logger::LogMessage> lines;
    std::vector<double> microseconds;
    microseconds.reserve(kSpawnCount);
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Debug);
        for (size_t spawn = 0; spawn < kSpawnCount; ++spawn)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto result = ModelEntityFactory::CreateFromModel(rs, world, model, modelGuid, "Piece");
            const auto stop = std::chrono::steady_clock::now();
            ASSERT_EQ(result.submeshEntities.size(), kPieceSubmeshCount);
            microseconds.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
        }
        Logger::Log::Flush();
    }

    std::vector<double> later(microseconds.begin() + 1, microseconds.end());
    std::sort(later.begin(), later.end());
    std::printf("[SpawnTiming] model 'Piece': %u submeshes, %u materials, %zu embedded images\n",
                model.GetMeshCount(), model.GetMaterialCount(), model.GetEmbeddedImages().size());
    std::printf("[SpawnTiming] first call %.1f us; calls 2..%zu: median %.1f us, p10 %.1f us, p90 %.1f us\n",
                microseconds.front(), kSpawnCount, later[later.size() / 2], later[later.size() / 10],
                later[later.size() * 9 / 10]);
    size_t modelLoadLines = 0;
    for (const auto& line : lines)
    {
        if (line.Message.find("[ModelLoad] 'Piece'") == std::string::npos)
            continue;
        if (modelLoadLines < 2)
            std::printf("[SpawnTiming] %s line %zu: %s\n", line.Level == Logger::LogLevel::Info ? "Info" : "Debug",
                        modelLoadLines + 1, line.Message.c_str());
        ++modelLoadLines;
    }
    std::printf("[SpawnTiming] %zu [ModelLoad] lines captured\n", modelLoadLines);

    device->Shutdown();
}
