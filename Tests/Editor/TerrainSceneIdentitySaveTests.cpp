// The scene save's terrain step: the document's own terrains take the saved scene's identity, and a
// terrain from a subscene keeps the subscene's.

#include "Terrain/TerrainZonePayloadSaver.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

namespace
{

class TerrainSceneIdentitySaveTest : public ::testing::Test
{
protected:
    void SetUp() override
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
        sceneDirectory = engine.GetAssetManager().GetAssetRoot() /
                         ("TerrainSceneIdentitySave_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(sceneDirectory);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(sceneDirectory, ec);
    }

    // A scene file on disk, as a save leaves it, and its path.
    std::filesystem::path Scene(const char* name)
    {
        const std::filesystem::path path = sceneDirectory / name;
        std::ofstream(path) << "{}";
        return path;
    }

    AssetRegistry& Registry() { return EngineCore::GetInstance().GetAssetManager().GetRegistry(); }

    ECS::EntityHandle AddTerrain(const GUID& scene)
    {
        Components::Terrain terrain{};
        terrain.BakeOriginScene = scene;
        const ECS::EntityHandle entity = world.CreateEntity();
        world.AddComponentImmediate(entity, terrain);
        return entity;
    }

    GUID IdentityOf(ECS::EntityHandle entity) { return world.GetComponent<Components::Terrain>(entity)->BakeOriginScene; }

    std::filesystem::path sceneDirectory;
    ECS::World world;
};

} // namespace

// Save As moves the document's terrains to the new scene's identity.
TEST_F(TerrainSceneIdentitySaveTest, SaveAsMovesTheDocumentsTerrainsToTheNewScene)
{
    const std::filesystem::path level = Scene("Level.scene");
    const ECS::EntityHandle terrain = AddTerrain(Registry().GetOrCreateAssetGUID(level));
    const std::filesystem::path copy = Scene("Copy.scene");

    Editor::AssignSavedSceneToTerrains(world, level, copy, Registry());

    const GUID copyScene = Registry().GetOrCreateAssetGUID(copy);
    ASSERT_FALSE(copyScene.IsNull());
    EXPECT_EQ(IdentityOf(terrain), copyScene);
}

// A terrain added since the document was last saved has no identity yet; the save gives it the
// saved scene's.
TEST_F(TerrainSceneIdentitySaveTest, ASaveStampsATerrainAddedSinceTheLastSave)
{
    const std::filesystem::path level = Scene("Level.scene");
    const ECS::EntityHandle terrain = AddTerrain(GUID{});

    Editor::AssignSavedSceneToTerrains(world, level, level, Registry());

    const GUID levelScene = Registry().GetOrCreateAssetGUID(level);
    ASSERT_FALSE(levelScene.IsNull());
    EXPECT_EQ(IdentityOf(terrain), levelScene);
}

// A terrain from a subscene carries the subscene file's identity. A save and a Save As of the
// parent document leave it there.
TEST_F(TerrainSceneIdentitySaveTest, ASaveOfTheParentKeepsASubsceneTerrainsIdentity)
{
    const GUID subscene = Registry().GetOrCreateAssetGUID(Scene("Sub.scene"));
    ASSERT_FALSE(subscene.IsNull());
    const ECS::EntityHandle terrain = AddTerrain(subscene);
    const std::filesystem::path parent = Scene("Parent.scene");

    Editor::AssignSavedSceneToTerrains(world, parent, parent, Registry());
    EXPECT_EQ(IdentityOf(terrain), subscene);
    Editor::AssignSavedSceneToTerrains(world, parent, Scene("ParentCopy.scene"), Registry());
    EXPECT_EQ(IdentityOf(terrain), subscene);
}
