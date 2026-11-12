#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Scene/SceneBackupRecovery.h"
#include "Terrain/Planet/PlanetHeightLayerSaver.h"
#include "Terrain/TerrainZonePayloadSaver.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PageStreaming/Height/HeightLayerStore.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "TerrainECS/TerrainService.h"

#include "TestTempDir.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
using GameEngine::TerrainECS::TerrainService;

constexpr uint32 kFaceSamples = 1025;
constexpr float32 kTexel = 1.0f; // meters, the level-0 texel of a terrain at one sample a meter

// A page the layer adds to: its address and what it composes over a zero base page, the bytes the
// page upload reads.
struct ComposedPage
{
    PageStreaming::PageAddress Address;
    std::vector<float32> Samples;

    bool operator==(const ComposedPage&) const = default;
};

// Every page of every level the layer adds to.
std::vector<ComposedPage> EveryPage(const PageStreaming::HeightPageLayer& layer)
{
    std::vector<ComposedPage> pages;
    std::vector<float32> samples(PageStreaming::kPageSampleCount);
    const std::span<const PageStreaming::PageStoreLevel> levels = layer.Levels();
    for (uint8 face = 0; face < layer.FaceCount(); ++face)
        for (uint8 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageStreaming::PageAddress address{face, level, x, z};
                    std::fill(samples.begin(), samples.end(), 0.0f);
                    if (layer.ApplyToPage(address, samples))
                        pages.push_back(ComposedPage{address, samples});
                }
    EXPECT_FALSE(pages.empty()) << "the layer adds to no page";
    return pages;
}

// FlushPlanetHeightLayer keeps the zones' contract for the planet's sculpt: Save mints
// <sceneStem>_Zones/<guid>.gepage, swaps Terrain.HeightLayerGuid to the file's GUID and quiesces; a
// backup stages `<live>.gepage.backup` for an existing file only, and the recovery promotes it.
class PlanetHeightLayerSaveTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        // A fresh service per test: the planet layers are service state.
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        m_Dir = TestUtils::MakeUniqueTempDirectory("GameEngine_PlanetHeightLayerSaveTests");
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Dir, ec);
    }

    // An active spherical planet with one sculpt dab.
    ECS::EntityHandle MakeSculptedPlanet(World& world)
    {
        const ECS::EntityHandle entity = world.CreateEntity();
        Components::Terrain terrain{};
        terrain.Domain = Components::TerrainDomain::Spherical;
        terrain.PlanetRadius = 2000.0f;
        // FindActiveTerrainEntity (the saver's resolver) needs a provisioned terrain.
        terrain.TerrainDataGeneration = 1;
        world.AddComponentImmediate(entity, Components::Transform{});
        world.AddComponentImmediate(entity, terrain);
        Dab(entity, {0.2f, 1.0f, 0.1f}, 2.0f);
        EXPECT_TRUE(TerrainService::Get().PlanetHeightLayerNeedsSave(entity.id));
        return entity;
    }

    static void Dab(ECS::EntityHandle planet, const Mathematics::Vector3& center, float32 amplitude)
    {
        TerrainService::Get().ApplyPlanetSculptDab(planet.id, kFaceSamples,
                                                   TerrainECS::PlanetSculptDab(center, 0.05f, amplitude));
    }

    static uint32 Flush(World& world, const std::filesystem::path& scenePath, Editor::ZonePayloadFlushMode mode,
                        Editor::ZonePayloadStagingLedger* ledger = nullptr,
                        Editor::EditorChangeNotifications* notifications = nullptr)
    {
        return Editor::FlushPlanetHeightLayer(world, scenePath, Registry(), TerrainService::Get(), mode, ledger,
                                              notifications);
    }

    static AssetRegistry& Registry() { return EngineCore::GetInstance().GetAssetManager().GetRegistry(); }

    static std::filesystem::path LayerPath(World& world, ECS::EntityHandle planet)
    {
        AssetMetadata meta{};
        const GUID guid = world.GetComponent<Components::Terrain>(planet)->HeightLayerGuid.ToGuid();
        return Registry().TryGetAssetMetadata(guid, meta) ? meta.Path : std::filesystem::path{};
    }

    std::filesystem::path m_Dir;
};

TEST_F(PlanetHeightLayerSaveTests, SaveMintsTheLayerInTheZonesFolderSwapsTheReferenceAndQuiesces)
{
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    const std::filesystem::path scenePath = m_Dir / "mint.scene";

    EXPECT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::SaveMintAndSwap), 1u);
    EXPECT_FALSE(TerrainService::Get().PlanetHeightLayerNeedsSave(planet.id));
    const std::filesystem::path layer = LayerPath(world, planet);
    ASSERT_FALSE(layer.empty()) << "the save swaps the reference to the file's GUID";
    EXPECT_TRUE(std::filesystem::exists(layer));
    EXPECT_EQ(layer.extension().string(), ".gepage");
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::equivalent(layer.parent_path(), Editor::SceneZonesFolderFor(scenePath), ec))
        << layer.parent_path();

    EXPECT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::SaveMintAndSwap), 0u) << "nothing new to save";
}

// The Save that sets Terrain.HeightLayerGuid rebuilds the Terrain inspector rows, so the Height Layer
// row shows the file without a new selection.
TEST_F(PlanetHeightLayerSaveTests, TheSaveThatSetsTheReferenceRebuildsTheTerrainInspector)
{
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    Editor::EditorChangeNotifications notifications;
    int rebuilds = 0;
    notifications.SubscribeComponentChanged(
        [&](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (e.entity == planet && e.componentType == ECS::GetComponentTypeId<Components::Terrain>() &&
                e.kind == Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild)
                ++rebuilds;
        });

    ASSERT_EQ(Flush(world, m_Dir / "rebuild.scene", Editor::ZonePayloadFlushMode::SaveMintAndSwap, nullptr,
                    &notifications),
              1u);
    EXPECT_EQ(rebuilds, 1);
}

// A save, then a reset and a load from the reference: every page of every level is the same, so the
// uploaded page bytes are the same.
TEST_F(PlanetHeightLayerSaveTests, AReloadedLayerHoldsEveryPageTheSavedSculptHeld)
{
    auto& service = TerrainService::Get();
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    Dab(planet, {1.0f, 1.0f, 0.2f}, -0.75f); // across a cube edge
    const std::filesystem::path scenePath = m_Dir / "reload.scene";
    ASSERT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::SaveMintAndSwap), 1u);
    const std::vector<ComposedPage> saved =EveryPage(*service.FindPlanetPageOverlay(planet.id)->Layer);

    service.ResetPlanetSculpt();
    ASSERT_EQ(service.FindPlanetPageOverlay(planet.id), nullptr);
    service.EnsurePlanetHeightLayerLoaded(planet.id, kFaceSamples,
                                          world.GetComponent<Components::Terrain>(planet)->HeightLayerGuid.ToGuid());
    const TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet.id);
    ASSERT_NE(overlay, nullptr);
    ASSERT_NE(overlay->Layer, nullptr);
    EXPECT_TRUE(EveryPage(*overlay->Layer) == saved) << "a page differs after the reload";
    EXPECT_FALSE(service.PlanetHeightLayerNeedsSave(planet.id)) << "a loaded layer is saved";
}

// The autosave stages a backup only for an existing layer file, once per sculpt version; the live file
// keeps its bytes; the recovery scan lists the backup and the promotion puts it in place of the file.
TEST_F(PlanetHeightLayerSaveTests, ABackupStagesAnExistingLayerOnlyAndTheRecoveryRestoresIt)
{
    auto& service = TerrainService::Get();
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    const std::filesystem::path scenePath = m_Dir / "backup.scene";
    Editor::ZonePayloadStagingLedger ledger;

    EXPECT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::BackupExistingOnly, &ledger), 0u);
    EXPECT_TRUE(world.GetComponent<Components::Terrain>(planet)->HeightLayerGuid.IsNull()) << "a backup never mints";

    ASSERT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::SaveMintAndSwap), 1u);
    const std::filesystem::path live = LayerPath(world, planet);
    const uintmax_t savedSize = std::filesystem::file_size(live);
    Dab(planet, {-0.3f, 1.0f, 0.4f}, 1.5f);
    const std::vector<ComposedPage> sculpted =EveryPage(*service.FindPlanetPageOverlay(planet.id)->Layer);

    EXPECT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::BackupExistingOnly, &ledger), 1u);
    EXPECT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::BackupExistingOnly, &ledger), 0u)
        << "the same sculpt version is staged once";
    const std::filesystem::path staged = Editor::StagedZonePayloadPathFor(live);
    EXPECT_TRUE(std::filesystem::exists(staged));
    EXPECT_EQ(std::filesystem::file_size(live), savedSize) << "a backup does not write the live file";
    EXPECT_TRUE(service.PlanetHeightLayerNeedsSave(planet.id)) << "a backup is not a save";

    const Editor::SceneRecoveryScan scan = Editor::ScanForSceneRecovery(scenePath);
    ASSERT_EQ(scan.StagedPayloads.size(), 1u);
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::equivalent(scan.StagedPayloads.front(), staged, ec));
    ASSERT_TRUE(Editor::PromoteStagedZonePayloads(scan.StagedPayloads));
    EXPECT_FALSE(std::filesystem::exists(staged));

    service.ResetPlanetSculpt();
    service.EnsurePlanetHeightLayerLoaded(planet.id, kFaceSamples,
                                          world.GetComponent<Components::Terrain>(planet)->HeightLayerGuid.ToGuid());
    const TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet.id);
    ASSERT_NE(overlay, nullptr);
    ASSERT_NE(overlay->Layer, nullptr);
    EXPECT_TRUE(EveryPage(*overlay->Layer) == sculpted) << "the promoted backup holds the sculpt after the save";
}

// A layer file that does not load is kept: the brush does not make a new sculpt over it, and a save
// neither writes over nor deletes the file.
TEST_F(PlanetHeightLayerSaveTests, ARefusedLayerFileIsNotReplacedByADabNorWrittenOverByASave)
{
    auto& service = TerrainService::Get();
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    const std::filesystem::path scenePath = m_Dir / "refused.scene";
    ASSERT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::SaveMintAndSwap), 1u);
    const std::filesystem::path live = LayerPath(world, planet);
    {
        std::ofstream damaged(live, std::ios::binary | std::ios::trunc);
        damaged << "not a page store";
    }
    const uintmax_t damagedSize = std::filesystem::file_size(live);
    const GUID reference = world.GetComponent<Components::Terrain>(planet)->HeightLayerGuid.ToGuid();

    service.ResetPlanetSculpt();
    service.EnsurePlanetHeightLayerLoaded(planet.id, kFaceSamples, reference);
    EXPECT_FALSE(service.PlanetSculptRefusal(planet.id, kFaceSamples, kTexel).empty());
    service.BeginPlanetSculptStroke(planet.id, kFaceSamples);
    Dab(planet, {0.2f, 1.0f, 0.1f}, 2.0f);
    const TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet.id);
    ASSERT_NE(overlay, nullptr);
    EXPECT_EQ(overlay->Layer, nullptr) << "a dab made a new sculpt in place of the refused file";
    EXPECT_FALSE(service.PlanetHeightLayerNeedsSave(planet.id));
    EXPECT_EQ(Flush(world, scenePath, Editor::ZonePayloadFlushMode::SaveMintAndSwap), 0u);
    ASSERT_TRUE(std::filesystem::exists(live)) << "the save deleted the refused file";
    EXPECT_EQ(std::filesystem::file_size(live), damagedSize) << "the save wrote over the refused file";
    EXPECT_EQ(world.GetComponent<Components::Terrain>(planet)->HeightLayerGuid.ToGuid(), reference);
}

// A height layer of one face is not a planet's sculpt: the load refuses it with the reason, and the
// brush does not make a new sculpt in its place.
TEST_F(PlanetHeightLayerSaveTests, AOneFaceLayerIsRefusedAndTheBrushKeepsItsPlace)
{
    auto& service = TerrainService::Get();
    World world;
    const ECS::EntityHandle planet = MakeSculptedPlanet(world);
    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(kFaceSamples, kFaceSamples);
    PageStreaming::HeightPageLayer oneFace(1u, levels);
    oneFace.WriteLevel0(0u, PageStreaming::PageSampleRect{10u, 10u, 11u, 11u}, std::vector<float32>(4u, 1.0f));
    const std::filesystem::path file = m_Dir / "oneface.gepage";
    ASSERT_EQ(PageStreaming::SaveHeightLayer(oneFace, file), "");
    const auto registered = Registry().RegisterAssetByPath(file);
    ASSERT_TRUE(registered.IsOk());

    service.ResetPlanetSculpt();
    service.EnsurePlanetHeightLayerLoaded(planet.id, kFaceSamples, registered.Value());
    EXPECT_NE(service.PlanetSculptRefusal(planet.id, kFaceSamples, kTexel).find("face count of 1"), std::string::npos);
    Dab(planet, {0.2f, 1.0f, 0.1f}, 2.0f);
    const TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet.id);
    ASSERT_NE(overlay, nullptr);
    EXPECT_EQ(overlay->Layer, nullptr) << "a dab made a new sculpt in place of the refused layer";
    EXPECT_FALSE(service.PlanetHeightLayerNeedsSave(planet.id));
}

} // namespace
