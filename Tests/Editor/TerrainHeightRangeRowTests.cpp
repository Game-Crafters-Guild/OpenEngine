// The terrain inspector's Height Range line: the planar terrain's height range in meters above its
// base (TerrainService::GetHeightRangeMeters), read-only, under the Height Scale row.
//
// BOUNDARY: the inspector is built in process over a live World with no terrain data, so the range
// is [0, Height Scale x the noise stack's RealizedTop]. The widening to the baked and streamed heights is covered by
// TerrainRegionBakeTests (TerrainHeightRange.*); the rebuild after a commit is not run.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainInspector.h"
#include "Terrain/TerrainRule/TerrainRuleConditionVocabulary.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

template<typename T>
T* FindFirst(UIElement& element)
{
    if (auto* found = dynamic_cast<T*>(&element))
        return found;
    for (const auto& child : element.GetChildren())
        if (T* found = FindFirst<T>(*child))
            return found;
    return nullptr;
}

// The inspector row whose label reads `text`: the row is the label's cell's parent.
UIElement* FindRow(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
        return label->GetParent() ? label->GetParent()->GetParent() : nullptr;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindRow(*child, text))
            return found;
    return nullptr;
}

void CollectLabelTexts(UIElement& element, std::vector<std::string>& texts)
{
    if (auto* label = dynamic_cast<Label*>(&element))
        texts.push_back(label->GetText());
    for (const auto& child : element.GetChildren())
        CollectLabelTexts(*child, texts);
}

// The text of the row's value: the label after the row's name.
std::string RowValueText(UIElement& row)
{
    std::vector<std::string> texts;
    CollectLabelTexts(row, texts);
    return texts.size() >= 2 ? texts[1] : std::string{};
}

class TerrainHeightRangeRowTest : public ::testing::Test
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
        // The row reads the range from the service. Other suites in this binary share it, so it stays up.
        if (!TerrainECS::TerrainService::IsInitialized())
            TerrainECS::TerrainService::Initialize();
    }

    // A Flat planar terrain under a Global volume whose Noise effect is Generated at `preset`.
    void BuildPlanar(float heightScale, float size,
                     Components::TerrainNoisePreset preset = Components::TerrainNoiseEffect{}.Preset)
    {
        RegisterTerrainInspector();
        Components::Terrain terrain{};
        terrain.Domain = Components::TerrainDomain::Planar;
        terrain.HeightScale = heightScale;
        terrain.SizeX = size;
        terrain.SizeZ = size;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        entity = world.Create(terrain).GetHandle();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Global;
        Components::TerrainNoiseEffect noise{};
        noise.Mode = Components::TerrainNoiseMode::Generated;
        noise.Preset = preset;
        noiseVolume = world.Create(volume, noise).GetHandle();
        Inspect();
    }

    void Inspect()
    {
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::Terrain>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.FrameRefreshCallbacks = &frameCallbacks;
        (*inspector)(ctx);
    }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    ECS::EntityHandle noiseVolume{};
    UIElement parent{"div"};
    std::vector<std::function<void()>> frameCallbacks;
};

TEST_F(TerrainHeightRangeRowTest, ThePlanarTerrainShowsItsHeightRangeInMetersReadOnly)
{
    BuildPlanar(100.0f, 1024.0f);
    UIElement* row = FindRow(parent, "Height Range");
    ASSERT_NE(row, nullptr) << "a planar terrain shows no Height range line";
    EXPECT_EQ(RowValueText(*row), "0 m to 100 m");
    EXPECT_EQ(FindFirst<FloatField>(*row), nullptr) << "the height range is derived, not authored";
}

// The noise rows belong to the Noise effect: the terrain's Base section has none.
TEST_F(TerrainHeightRangeRowTest, TheBaseSectionHasNoNoiseRows)
{
    BuildPlanar(100.0f, 1024.0f);
    EXPECT_EQ(FindRow(parent, "Noise Preset"), nullptr);
}

TEST_F(TerrainHeightRangeRowTest, TheHeightRangeFollowsTheHeightScale)
{
    BuildPlanar(1.0f, 1024.0f);
    UIElement* row = FindRow(parent, "Height Range");
    ASSERT_NE(row, nullptr) << "a planar terrain shows no Height range line";
    EXPECT_EQ(RowValueText(*row), "0 m to 1 m");
}

// Before its tiles exist, a 250 km noise terrain shows the heights its large octaves reach.
TEST_F(TerrainHeightRangeRowTest, ALargeNoiseTerrainShowsItsRealizedTopBeforeItsTilesExist)
{
    constexpr float kHeightScale = 100.0f;
    constexpr float kSize = 250000.0f;
    BuildPlanar(kHeightScale, kSize);
    UIElement* row = FindRow(parent, "Height Range");
    ASSERT_NE(row, nullptr) << "a planar terrain shows no Height range line";
    const Components::Terrain* terrain = world.GetComponent<Components::Terrain>(entity);
    ASSERT_NE(terrain, nullptr);
    const float32 realizedTop =
        TerrainECS::GeneratedNoiseStackOf(*terrain, {Components::TerrainNoiseEffect{}.Preset}).RealizedTop();
    ASSERT_GT(realizedTop, 10.0f);
    EXPECT_EQ(RowValueText(*row), Editor::TerrainRuleVocabulary::FormatHeightRange({0.0f, kHeightScale * realizedTop}));
}

// The extraction system provisions the tiles again after a Noise Preset pick on the effect. The Height Range row
// shows the new range on the next frame, without a reselect.
TEST_F(TerrainHeightRangeRowTest, TheHeightRangeFollowsTheTilesProvisionedAfterANoisePresetPick)
{
    auto& service = TerrainECS::TerrainService::Get();
    BuildPlanar(64.0f, 2048.0f, Components::TerrainNoisePreset::Rugged);
    auto* terrain = world.GetComponentForWrite<Components::Terrain>(entity);
    ASSERT_NE(terrain, nullptr);
    TerrainECS::TiledTerrainHandle tiles = service.CreateTiledTerrain(service.BuildTiledTerrainConfig(*terrain, "Rugged", TerrainECS::GeneratedNoiseRelief{Components::TerrainNoisePreset::Rugged}));
    terrain->TiledTerrainHandle = tiles.Index;
    terrain->TiledTerrainGeneration = tiles.Generation;
    frameCallbacks.clear();
    parent.RemoveAllChildren();
    Inspect();
    UIElement* row = FindRow(parent, "Height Range");
    ASSERT_NE(row, nullptr);
    const std::string ruggedText = RowValueText(*row);

    auto* noise = world.GetComponentForWrite<Components::TerrainNoiseEffect>(noiseVolume);
    ASSERT_NE(noise, nullptr);
    noise->Preset = Components::TerrainNoisePreset::Smooth;
    service.DestroyTiledTerrain(tiles);
    tiles = service.CreateTiledTerrain(service.BuildTiledTerrainConfig(*terrain, "Smooth", TerrainECS::GeneratedNoiseRelief{Components::TerrainNoisePreset::Smooth}));
    terrain->TiledTerrainHandle = tiles.Index;
    terrain->TiledTerrainGeneration = tiles.Generation;
    for (const auto& callback : frameCallbacks)
        callback();

    const TerrainECS::TerrainHeightRangeMeters smooth = service.GetHeightRangeMeters(*terrain, TerrainECS::GeneratedNoiseRelief{Components::TerrainNoisePreset::Smooth});
    const std::string smoothText = Editor::TerrainRuleVocabulary::FormatHeightRange({smooth.Min, smooth.Max});
    EXPECT_NE(smoothText, ruggedText) << "the presets reach the same range: the test proves nothing";
    EXPECT_EQ(RowValueText(*row), smoothText);
    service.DestroyTiledTerrain(tiles);
}

} // namespace
