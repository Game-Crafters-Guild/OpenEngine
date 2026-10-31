#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Editor/Registries/EditorMenuRegistry.h"
#include "Panels/SaveSceneAsModal.h"
#include "Scene/DefaultSceneEntities.h"
#include "Scene/SceneEditorController.h"
#include "ScopedEditorUserDataRoot.h"

#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/Button.h"
#include "UI/UIElement.h"

#include "TestDeviceHelper.h"
#include "TestTempDir.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
std::map<std::string, bool> SceneEntities(ECS::World& world)
{
    std::map<std::string, bool> entities;
    world.Query<ECS::Read<Components::Name>>().Each(
        [&](ECS::EntityHandle entity, const Components::Name& name)
        {
            entities.emplace(name.value, world.GetComponent<Components::MeshRenderer>(entity) != nullptr);
        });
    return entities;
}

Button* FindSaveButton(UIElement& element)
{
    if (auto* button = dynamic_cast<Button*>(&element); button && button->HasClass("primary"))
        return button;
    for (const auto& child : element.GetChildren())
    {
        if (auto* button = FindSaveButton(*child))
            return button;
    }
    return nullptr;
}

class NewSceneSeedTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
        ASSERT_EQ(engine.GetRenderServices(), nullptr);
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(m_RenderServices.Initialize(m_Device.get()));
        ASSERT_TRUE(engine.EnableRenderingLoop(&m_RenderServices));
        ASSERT_EQ(engine.GetRenderServices(), &m_RenderServices);
        std::filesystem::create_directories(m_Project.Path() / "Assets");
        m_Controller.AttachToRoot(m_Root, m_World, m_Notifications, nullptr);
    }

    void TearDown() override
    {
        EngineCore::GetInstance().DisableRenderingLoop();
        m_World.Clear();
        m_RenderServices.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
    }

    TestUtils::ScopedTempDir m_Project{TestUtils::MakeUniqueTempDirectory("ge_new_scene_seed")};
    ScopedEditorUserDataRoot m_UserData{m_Project.Path() / "UserData"};
    std::unique_ptr<Rendering::IDevice> m_Device;
    Engine::Renderer::RenderServices m_RenderServices;
    ECS::World m_World;
    Editor::EditorChangeNotifications m_Notifications;
    UIElement m_Root{"TestRoot"};
    Editor::SceneEditorController m_Controller;
};
} // namespace

TEST_F(NewSceneSeedTests, FileMenuListsSceneActionsInOrder)
{
    std::vector<std::string> paths;
    for (const auto& item : Editor::EditorMenuRegistry::Get().Snapshot())
    {
        if (item.Path.starts_with("File/") && item.Path != "File/Exit")
            paths.push_back(item.Path);
    }
    EXPECT_EQ(paths, (std::vector<std::string>{"File/New Scene", "File/Load Scene", "File/Save Scene"}));
}

TEST_F(NewSceneSeedTests, FileNewSceneUsesTheDirtyScenePrompt)
{
    m_Controller.RequestNewScene();
    m_Controller.Update(nullptr);
    m_Controller.MarkSceneDirty();
    bool invoked = false;
    for (const auto& item : Editor::EditorMenuRegistry::Get().Snapshot())
    {
        if (item.Path == "File/New Scene")
            invoked = Editor::EditorMenuRegistry::Get().TryInvoke(item.CommandId);
    }
    ASSERT_TRUE(invoked);
    m_Controller.Update(nullptr);
    EXPECT_EQ(m_Controller.GetActiveModalKind(), "saveChanges");
    EXPECT_TRUE(m_Controller.IsSceneDirty());
}

TEST_F(NewSceneSeedTests, FileSaveSceneNamesAnUntitledScene)
{
    m_Controller.RequestNewScene();
    m_Controller.Update(nullptr);
    bool invoked = false;
    for (const auto& item : Editor::EditorMenuRegistry::Get().Snapshot())
    {
        if (item.Path == "File/Save Scene")
            invoked = Editor::EditorMenuRegistry::Get().TryInvoke(item.CommandId);
    }
    ASSERT_TRUE(invoked);
    EXPECT_EQ(m_Controller.GetActiveModalKind(), "saveAs");
}

TEST_F(NewSceneSeedTests, HierarchyAndPickerSeedTheSameEntitiesAsStartup)
{
    ECS::World startup;
    Editor::SeedDefaultSceneEntities(startup, &m_RenderServices);
    const auto expected = SceneEntities(startup);
    const std::map<std::string, bool> authored{
        {"Directional Light", false}, {"Main Camera", false}, {"Mirror Sphere", true},
        {"Ground", true}, {"Post Process Volume", false}, {"Sky Environment", false}, {"Sphere", true}};
    ASSERT_EQ(expected, authored) << "positive control: startup has all three primitives";

    m_Controller.RequestNewScene();
    m_Controller.Update(nullptr);
    EXPECT_EQ(SceneEntities(m_World), expected) << "Hierarchy > New Scene";
    EXPECT_FALSE(m_Controller.IsSceneDirty());

    m_Controller.OpenProjectStartupScene(m_Project.Path(), false);
    m_Controller.Update(nullptr);
    EXPECT_EQ(SceneEntities(m_World), expected) << "picker project with no startup scene";
    EXPECT_FALSE(m_Controller.IsSceneDirty());

    m_Controller.SetEagerScenePersistence(true);
    m_Controller.RequestNewScene();
    ASSERT_EQ(m_Controller.GetActiveModalKind(), "saveAs");
    SaveSceneAsModal* modal = nullptr;
    for (const auto& child : m_Root.GetChildren())
    {
        if (auto* saveAs = dynamic_cast<SaveSceneAsModal*>(child.get()))
            modal = saveAs;
    }
    ASSERT_NE(modal, nullptr);
    modal->Show(m_Project.Path() / "Assets");
    auto* save = FindSaveButton(*modal);
    ASSERT_NE(save, nullptr);
    save->TriggerClick();
    EXPECT_EQ(SceneEntities(m_World), expected) << "New Scene after committing its name";
    EXPECT_EQ(m_Controller.GetActiveScenePath(), m_Project.Path() / "Assets/NewScene.scene");
    EXPECT_FALSE(m_Controller.IsSceneDirty());
}
