// Load the bytes shipped with the picker, including every template in its catalog.
#include <gtest/gtest.h>

#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ECSTemplates.h"
#include "Projects/ProjectCatalog.h"
#include "Scene/SceneIO.h"
#include "StagedTestPaths.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

using namespace GameEngine;

namespace
{
std::filesystem::path TemplateRoot()
{
    return TestPaths::StagedRoot() / "Apps/Editor/ProjectTemplates";
}

// One project mount, backed by the same registry used by the editor's resolver.
class ShippedTemplateScene : public testing::TestWithParam<const char*>, public Scene::ISceneAssetResolver
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(Registry.Initialize(GetAssetRoot()));
    }

    std::filesystem::path GetAssetRoot() const override { return TemplateRoot() / GetParam() / "Assets"; }
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const override
    {
        return path.is_absolute() ? path : GetAssetRoot() / path;
    }
    GUID ResolveGuid(const GUID& guid) const override { return Registry.ResolveGuid(guid); }
    GUID GetOrCreateAssetGuid(const std::filesystem::path& path) override
    {
        const auto result = Registry.RegisterAssetByPath(path);
        return result.IsOk() ? result.Value() : GUID::Null();
    }
    bool TryGetPathAndType(const GUID& guid, std::filesystem::path& path, AssetType& type) const override
    {
        AssetMetadata metadata{};
        if (!Registry.TryGetAssetMetadata(guid, metadata))
            return false;
        path = metadata.Path;
        type = metadata.Type;
        return true;
    }
    bool TryGetGuidAndType(const std::filesystem::path& path, GUID& guid, AssetType& type) const override
    {
        AssetMetadata metadata{};
        if (!Registry.TryGetAssetMetadata(path, metadata))
            return false;
        guid = metadata.Guid;
        type = metadata.Type;
        return true;
    }

    // Loads the template's Main.scene into the world and reports every field the loader skipped.
    void LoadMainScene(ECS::World& world)
    {
        const auto projectRoot = TemplateRoot() / GetParam();
        const auto scenePath = projectRoot / "Assets/Scenes/Main.scene";
        ASSERT_TRUE(std::filesystem::is_regular_file(scenePath)) << scenePath;

        Scene::SceneLoadDegradation degradation;
        Scene::LoadOptions options;
        options.assetResolver = this;
        options.assetRootOverride = projectRoot / "Assets";
        options.projectRoot = projectRoot;
        options.outDegradation = &degradation;
        ASSERT_TRUE(Scene::LoadSceneFromFile(world, scenePath, options))
            << Scene::GetLastSceneIOError().message;
        for (const auto& skip : degradation.skips)
            ADD_FAILURE() << skip.file << ':' << skip.line << ' ' << skip.entityId << '.'
                          << skip.component << '.' << skip.field << ": " << skip.message;
    }

    AssetRegistry Registry;
};
} // namespace

TEST(ShippedTemplateCatalog, EveryTemplateHasALoadExpectation)
{
    const auto manifestPath = TemplateRoot() / "manifest.json";
    std::ifstream file(manifestPath);
    ASSERT_TRUE(file.is_open()) << manifestPath;
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Editor::ProjectCatalog catalog;
    std::string error;
    ASSERT_TRUE(Editor::ParseProjectCatalog(text, {}, catalog, &error)) << error;
    std::set<std::string> paths;
    for (const auto& entry : catalog.Entries)
    {
        EXPECT_EQ(entry.Source.Type, Editor::ProjectSourceType::Local) << entry.Id;
        EXPECT_TRUE(paths.insert(entry.Source.Path).second) << entry.Id;
    }
    EXPECT_EQ(paths, (std::set<std::string>{"Blank", "Basic3D"}));
}

TEST_P(ShippedTemplateScene, LoadsTheAuthoredEntitySetWithoutDegradation)
{
    ECS::World world;
    LoadMainScene(world);
    if (HasFatalFailure())
        return;

    // Scene tag -> (display name, mesh presence), independent of the loaded data.
    using EntitySet = std::map<std::string, std::pair<std::string, bool>>;
    EntitySet expected{
        {"physics_settings", {"Physics Settings", false}},
        {"directional_light", {"Directional Light", false}},
        {"sky_environment", {"Sky Environment", false}},
        {"main_camera", {"Main Camera", false}},
        {"post_process_volume", {"Post Process Volume", false}},
    };
    if (std::string_view(GetParam()) == "Basic3D")
    {
        expected.insert({{"plane", {"Ground", true}}, {"cube", {"Cube Left", true}},
                         {"cube_2", {"Cube Center", true}}, {"cube_3", {"Cube Right", true}},
                         {"cube_4", {"Cube Stacked", true}}});
    }
    EntitySet actual;
    auto entities = world.Query<ECS::Read<Components::SceneEntityTag>, ECS::Read<Components::Name>>();
    entities.IncludeDisabled();
    entities.Each([&](ECS::EntityHandle entity, const Components::SceneEntityTag& tag,
                     const Components::Name& name)
    {
        EXPECT_TRUE(actual.emplace(std::string(tag.View()),
            std::make_pair(std::string(name.View()), world.HasComponent<Components::MeshRenderer>(entity))).second);
    });
    EXPECT_EQ(world.GetEntityCount(), expected.size());
    EXPECT_EQ(actual, expected);

    // The Basic 3D ground is the engine's grid ground, the material a new scene's ground uses.
    if (std::string_view(GetParam()) == "Basic3D")
    {
        const GUID gridGround =
            AssetRegistry::DeriveGuidForSourcePath(kAssetSourceAliasEditor, "Materials/GroundGrid.material");
        int grounds = 0;
        world.Query<ECS::Read<Components::SceneEntityTag>, ECS::Read<Components::MeshRenderer>>().Each(
            [&](ECS::EntityHandle, const Components::SceneEntityTag& tag, const Components::MeshRenderer& renderer)
            {
                if (tag.View() != "plane")
                    return;
                ++grounds;
                EXPECT_EQ(renderer.materialAssetGuid.ToGuid(), gridGround);
            });
        EXPECT_EQ(grounds, 1);
    }
}

// Every template opens on the tuned look: the sun at 10:00 at 40 degrees north, and the horizon
// haze on the global volume. The templates author neither; both come from the component defaults.
TEST_P(ShippedTemplateScene, OpensOnTheMidMorningSunAndTheHorizonHaze)
{
    ECS::World world;
    LoadMainScene(world);
    if (HasFatalFailure())
        return;

    int skies = 0;
    world.Query<ECS::Read<Components::SkyEnvironment>>().Each(
        [&](ECS::EntityHandle, const Components::SkyEnvironment& sky)
        {
            ++skies;
            EXPECT_FLOAT_EQ(sky.TimeOfDayHours, 10.0f);
            EXPECT_FLOAT_EQ(sky.Latitude, 40.0f);
        });
    EXPECT_EQ(skies, 1);

    int fogs = 0;
    world.Query<ECS::Read<Components::PostProcessVolume>, ECS::Read<Components::HeightFogEffect>>().Each(
        [&](ECS::EntityHandle, const Components::PostProcessVolume& volume, const Components::HeightFogEffect& fog)
        {
            ++fogs;
            EXPECT_TRUE(volume.IsGlobal);
            EXPECT_TRUE(fog.Enabled);
            EXPECT_FLOAT_EQ(fog.Intensity, 1.0f);
            EXPECT_FLOAT_EQ(fog.Density, 1.0f);
            EXPECT_FLOAT_EQ(fog.MaxOpacity, 1.0f);
            EXPECT_FLOAT_EQ(fog.MinDistance, 150.0f);
            EXPECT_FLOAT_EQ(fog.SmoothLength, 150.0f);
            EXPECT_FLOAT_EQ(fog.MaxDistance, 3000.0f);
            EXPECT_FALSE(fog.HeightFogEnabled);
            EXPECT_EQ(fog.ColorMode, Components::HeightFogColorMode::SkyHorizon);
            EXPECT_FLOAT_EQ(fog.SkyFillEnd, 0.05f);
        });
    EXPECT_EQ(fogs, 1) << "the global post-process volume carries the height fog";
}

INSTANTIATE_TEST_SUITE_P(BuiltIn, ShippedTemplateScene, testing::Values("Blank", "Basic3D"),
                        [](const testing::TestParamInfo<const char*>& info) { return info.param; });
