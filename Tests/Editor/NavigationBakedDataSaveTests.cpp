// The scene save's navigation step: each baked mesh surface writes its own .navsurface beside the
// scene, a copied surface and a scene saved under a new name included.

#include "Navigation/NavigationBakedDataSave.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "Components/Transform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace GameEngine;

namespace
{

using Components::NavigationSurface;

class NavigationBakedDataSaveTest : public ::testing::Test
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
                         ("NavigationBakedDataSave_" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(sceneDirectory);
        PathfindingECS::NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        PathfindingECS::RegisterNavigationWorldHooks(*world);

        auto ground = world->Create();
        Components::PhysicsBody body{};
        body.motionType = Physics::MotionType::Static;
        ground.Set(body);
        ground.Set(Components::PhysicsCollider{});
        Components::PlaneColliderShape plane{};
        plane.halfExtent = 100.0f;
        ground.Set(plane);
        ground.Set(Components::Transform{});
        ground.Set(Components::WorldTransform{});
        first = CreateSurface();
        second = CreateSurface();
        PathfindingECS::NavigationSurfaceSystem system;
        // Tiles build on the engine's workers: update until both surfaces are baked.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline &&
               (Surface(first).BakePhase != Components::NavigationBakePhase::Baked ||
                Surface(second).BakePhase != Components::NavigationBakePhase::Baked))
        {
            system.Update(*world, 1.0f / 60.0f);
            world->ProcessCommands();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(Surface(first).BakePhase, Components::NavigationBakePhase::Baked);
        ASSERT_EQ(Surface(second).BakePhase, Components::NavigationBakePhase::Baked);
    }

    void TearDown() override
    {
        world.reset();
        PathfindingECS::NavigationService::Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(sceneDirectory, ec);
    }

    ECS::EntityHandle CreateSurface()
    {
        NavigationSurface surface{};
        surface.FitBounds = false;
        surface.Extent = {16.0f, 10.0f, 16.0f};
        const ECS::EntityHandle entity = world->Create(surface).GetHandle();
        world->ProcessCommands();
        return entity;
    }

    const NavigationSurface& Surface(ECS::EntityHandle entity) { return *world->GetComponent<NavigationSurface>(entity); }

    std::filesystem::path FileOf(ECS::EntityHandle entity)
    {
        AssetMetadata metadata{};
        EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(
            Surface(entity).BakedData.ToGuid(), metadata);
        return metadata.Path;
    }

    std::filesystem::path sceneDirectory;
    std::unique_ptr<ECS::World> world;
    ECS::EntityHandle first;
    ECS::EntityHandle second;
};

} // namespace

// A surface whose BakedData names another surface's file, as a copied surface's does, saves to a
// file of its own; the original keeps its file.
TEST_F(NavigationBakedDataSaveTest, ACopiedSurfaceSavesToItsOwnFile)
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path scene = sceneDirectory / "Level.scene";
    EXPECT_EQ(Editor::WriteNavigationBakedDataForSave(*world, scene, assets), 2u);
    const std::filesystem::path firstFile = FileOf(first);
    ASSERT_FALSE(firstFile.empty());

    world->GetComponentForWrite<NavigationSurface>(second)->BakedData = Surface(first).BakedData;
    Editor::WriteNavigationBakedDataForSave(*world, scene, assets);

    EXPECT_EQ(FileOf(first), firstFile);
    EXPECT_NE(FileOf(second), firstFile);
    EXPECT_TRUE(std::filesystem::exists(FileOf(second)));
}

// Saving the scene under a new name gives its surfaces files named after the new scene; the old
// scene's files stay as they are.
TEST_F(NavigationBakedDataSaveTest, SaveAsWritesFilesOfTheNewScene)
{
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    Editor::WriteNavigationBakedDataForSave(*world, sceneDirectory / "Level.scene", assets);
    const std::filesystem::path original = FileOf(first);

    Editor::WriteNavigationBakedDataForSave(*world, sceneDirectory / "Copy.scene", assets);

    EXPECT_NE(FileOf(first), original);
    // The registry may report the path case-folded.
    std::string name = FileOf(first).filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    EXPECT_TRUE(name.starts_with("copy-")) << name;
    EXPECT_TRUE(std::filesystem::exists(original));
}
