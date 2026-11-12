#include <gtest/gtest.h>

#include "Editor/Assets/BuiltInAssetOpeners.h"
#include "Editor/Registries/AssetOpenerRegistry.h"

#include <filesystem>
#include <string>
#include <string_view>

using GameEngine::Editor::AnimationAssetWindow;
using GameEngine::Editor::AssetOpenerDescriptor;
using GameEngine::Editor::AssetOpenerRegistry;
using GameEngine::Editor::AssetOpenPreferences;
using GameEngine::Editor::BuiltInAssetOpenActions;

namespace
{

// The editor's own openers over actions that record the window each open reached.
class BuiltInAssetOpenersTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        BuiltInAssetOpenActions actions;
        actions.ReadPreferences = [this](const std::filesystem::path&) { return m_Preferences; };
        actions.OpenScene = [this](const std::filesystem::path&) { return Reach("Scene"); };
        actions.OpenInScriptEditor = [this](const std::filesystem::path&) { return Reach("ScriptEditor"); };
        actions.OpenInExternalScriptEditor = [this](const std::filesystem::path&)
        { return Reach("ExternalScriptEditor"); };
        actions.OpenInIde = [this](const std::filesystem::path&) { return Reach("ExternalIde"); };
        actions.OpenInNodeGraph = [this](const std::filesystem::path&) { return Reach("NodeGraph"); };
        actions.ShowInInspector = [this](const std::filesystem::path&) { return Reach("Inspector"); };
        actions.OpenInAnimationWindow = [this](const std::filesystem::path&, AnimationAssetWindow window)
        {
            return Reach(window == AnimationAssetWindow::Timeline     ? "Timeline"
                         : window == AnimationAssetWindow::ClipEditor ? "ClipEditor"
                                                                      : "Animation");
        };
        GameEngine::Editor::RegisterBuiltInAssetOpeners(m_Registry, std::move(actions));
    }

    // The window the file reached, or "OperatingSystem" when no opener took it.
    std::string OpenTarget(const std::filesystem::path& path)
    {
        m_Reached.clear();
        if (!m_Registry.Open(path))
            return "OperatingSystem";
        return m_Reached;
    }

    bool Reach(const char* target)
    {
        m_Reached = target;
        return m_Succeeds;
    }

    AssetOpenerRegistry m_Registry;
    AssetOpenPreferences m_Preferences;
    std::string m_Reached;
    bool m_Succeeds = true;
};

} // namespace

TEST(AssetOpenerRegistryTests, ARegisteredExtensionOpensThroughItsOpener)
{
    AssetOpenerRegistry registry;
    std::filesystem::path opened;
    registry.Register({"scribe", [](std::string_view ext) { return ext == ".scribe"; },
                       [&opened](const std::filesystem::path& path)
                       {
                           opened = path;
                           return true;
                       }});
    EXPECT_TRUE(registry.Open("Assets/AI/Guard.SCRIBE"));
    EXPECT_EQ(opened, std::filesystem::path("Assets/AI/Guard.SCRIBE"));
}

TEST(AssetOpenerRegistryTests, AnUnregisteredExtensionIsLeftToTheOperatingSystem)
{
    AssetOpenerRegistry registry;
    bool called = false;
    registry.Register({"scribe", [](std::string_view ext) { return ext == ".scribe"; },
                       [&called](const std::filesystem::path&) { return called = true; }});
    EXPECT_FALSE(registry.Open("Assets/Textures/Rock.png"));
    EXPECT_FALSE(registry.Open(""));
    EXPECT_FALSE(called);
}

TEST(AssetOpenerRegistryTests, ARegistrationWithTheSameIdReplacesTheOpenerInPlace)
{
    AssetOpenerRegistry registry;
    std::string reached;
    const auto any = [](std::string_view) { return true; };
    registry.Register({"first", any, [&reached](const std::filesystem::path&) { reached = "old"; return true; }});
    registry.Register({"second", any, [&reached](const std::filesystem::path&) { reached = "second"; return true; }});
    registry.Register({"first", any, [&reached](const std::filesystem::path&) { reached = "new"; return true; }});
    EXPECT_TRUE(registry.Open("a.txt"));
    EXPECT_EQ(reached, "new");
}

TEST_F(BuiltInAssetOpenersTest, MaterialsOpenInTheInspector)
{
    EXPECT_EQ(OpenTarget("a.material"), "Inspector");
    EXPECT_EQ(OpenTarget("a.mat"), "Inspector");
}

TEST_F(BuiltInAssetOpenersTest, HandWrittenGlslOpensInTheScriptEditor)
{
    m_Preferences.GlslOpensInMaterialGraph = false;
    EXPECT_EQ(OpenTarget("a.glsl"), "ScriptEditor");
}

TEST_F(BuiltInAssetOpenersTest, ShaderGraphGlslOpensInTheNodeGraph)
{
    m_Preferences.GlslOpensInMaterialGraph = true;
    EXPECT_EQ(OpenTarget("a.glsl"), "NodeGraph");
}

TEST_F(BuiltInAssetOpenersTest, CSharpFollowsTheScriptEditorPreference)
{
    m_Preferences.OpenCSharpInScriptEditor = true;
    EXPECT_EQ(OpenTarget("a.cs"), "ScriptEditor");
    m_Preferences.OpenCSharpInScriptEditor = false;
    EXPECT_EQ(OpenTarget("a.cs"), "ExternalScriptEditor");
}

TEST_F(BuiltInAssetOpenersTest, NativeSourceFollowsItsOwnPreference)
{
    m_Preferences.OpenNativeSourceInScriptEditor = false;
    EXPECT_EQ(OpenTarget("a.cpp"), "ExternalIde");
    EXPECT_EQ(OpenTarget("a.h"), "ExternalIde");
    m_Preferences.OpenNativeSourceInScriptEditor = true;
    EXPECT_EQ(OpenTarget("a.cpp"), "ScriptEditor");
}

TEST_F(BuiltInAssetOpenersTest, ScenesAndAnimationAssetsReachTheirPanels)
{
    EXPECT_EQ(OpenTarget("a.scene"), "Scene");
    EXPECT_EQ(OpenTarget("a.timeline"), "Timeline");
    EXPECT_EQ(OpenTarget("a.clipset"), "ClipEditor");
    EXPECT_EQ(OpenTarget("a.anim"), "Animation");
    EXPECT_EQ(OpenTarget("a.glb"), "Animation");
}

TEST_F(BuiltInAssetOpenersTest, AnythingElseGoesToTheOperatingSystem)
{
    EXPECT_EQ(OpenTarget("a.png"), "OperatingSystem");
    EXPECT_EQ(OpenTarget("a.txt"), "OperatingSystem");
    EXPECT_EQ(OpenTarget(""), "OperatingSystem");
}

TEST_F(BuiltInAssetOpenersTest, AFailedOpenGoesToTheOperatingSystem)
{
    m_Succeeds = false;
    EXPECT_EQ(OpenTarget("a.scene"), "OperatingSystem");
}
