// Opening another project from the picker asks to save a dirty scene first: Cancel keeps the
// current project, and Don't Save switches without asking again when the project's startup scene
// replaces the discarded one.

#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Scene/SceneEditorController.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "UI/UIElement.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace
{
using namespace GameEngine;
namespace fs = std::filesystem;

class SaveBeforeProjectSwitchTests : public ::testing::Test
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
        m_Controller.AttachToRoot(m_Root, m_World, m_Notifications, nullptr);
        m_World.CreateEntity();
        m_Controller.MarkSceneDirty();
        ASSERT_TRUE(m_Controller.IsSceneDirty());
    }

    ECS::World m_World;
    Editor::EditorChangeNotifications m_Notifications;
    UIElement m_Root{"TestRoot"};
    Editor::SceneEditorController m_Controller;
};
} // namespace

TEST_F(SaveBeforeProjectSwitchTests, CancelKeepsTheCurrentProject)
{
    int switches = 0;
    int cancels = 0;
    m_Controller.PromptSaveBefore("opening another project", [&switches](bool) { ++switches; },
                                  [&cancels]() { ++cancels; });
    ASSERT_EQ(m_Controller.GetActiveModalKind(), "saveChanges");

    std::string error;
    ASSERT_TRUE(m_Controller.RespondToActiveModal("cancel", &error)) << error;
    m_Controller.Update(nullptr);
    EXPECT_EQ(switches, 0) << "Cancel switched the project";
    EXPECT_EQ(cancels, 1);
    EXPECT_TRUE(m_Controller.IsSceneDirty());
}

TEST_F(SaveBeforeProjectSwitchTests, DontSaveSwitchesAndTheStartupSceneDoesNotAskAgain)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path project = fs::temp_directory_path() / ("ge_switch_dontsave_" + std::to_string(stamp));
    fs::create_directories(project / "Assets");

    int switches = 0;
    bool changesDiscarded = false;
    m_Controller.PromptSaveBefore(
        "opening another project",
        [&switches, &changesDiscarded](bool discarded)
        {
            ++switches;
            changesDiscarded = discarded;
        },
        {});
    ASSERT_EQ(m_Controller.GetActiveModalKind(), "saveChanges");

    std::string error;
    ASSERT_TRUE(m_Controller.RespondToActiveModal("dontSave", &error)) << error;
    EXPECT_EQ(switches, 1);
    EXPECT_TRUE(changesDiscarded);

    // The project names no scene, so its open is a new default scene in place of the discarded one.
    m_Controller.OpenProjectStartupScene(project, changesDiscarded);
    m_Controller.Update(nullptr);
    EXPECT_TRUE(m_Controller.GetActiveModalKind().empty()) << "the discarded scene was asked about twice";
    EXPECT_FALSE(m_Controller.IsSceneDirty());

    std::error_code ec;
    fs::remove_all(project, ec);
}

// Don't Save answers the action it was asked for. When that action ends without changing the scene
// (a quit that a later prompt cancels, a switch that is refused), the next scene change still asks.
TEST_F(SaveBeforeProjectSwitchTests, DontSaveForAnActionThatEndsEarlyStillAsksAtTheNextSceneChange)
{
    m_Controller.PromptSaveBefore("quitting", [](bool) {}, {});
    std::string error;
    ASSERT_TRUE(m_Controller.RespondToActiveModal("dontSave", &error)) << error;

    m_Controller.RequestNewScene();
    m_Controller.Update(nullptr);
    EXPECT_EQ(m_Controller.GetActiveModalKind(), "saveChanges") << "the dirty scene was replaced without asking";
    EXPECT_TRUE(m_Controller.IsSceneDirty());
}
