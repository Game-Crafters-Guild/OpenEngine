// The SceneLoader script API through its generated table, the calls a C# script makes
// (Managed/Scripting.Bindings/Generated/SceneLoader.g.cs), on a headless engine: a load is polled to
// ready and revealed when the game releases it, and an operation that a newer script load replaced is
// refused with the fix, while one that a native load replaced stays readable.
#include "AssetCore/GUID.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Hosting/RuntimeHost.h"
#include "Engine/Loading/SceneLoader.h"
#include "Scripting/ScriptingABI.h"
#include "Scripting/ScriptsConfig.h"
#include "scripting/SceneLoader.script.gen.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace fs = std::filesystem;
using ScriptOperation = Loading::Script::SceneLoadOperation;
using ScriptRevealPolicy = Loading::Script::SceneRevealPolicy;

namespace
{
constexpr int kFrameCap = 600;

class ScriptLoadApplication final : public Application
{
  public:
    using Application::Application;
};

const GE_SceneLoader_Table_v1& Table()
{
    const void* table = nullptr;
    uint32_t size = 0;
    EXPECT_EQ(GE_SceneLoader_GetInterface(GE_SceneLoader_Hash, &table, &size), GE_Result_Ok);
    EXPECT_EQ(size, sizeof(GE_SceneLoader_Table_v1));
    return *static_cast<const GE_SceneLoader_Table_v1*>(table);
}

ScriptOperation ScriptLoad(std::string_view scene, ScriptRevealPolicy reveal, bool pauseWorldDrawing = false)
{
    ScriptOperation load{};
    EXPECT_EQ(Table().Load(scene.data(), static_cast<int32_t>(scene.size()), reveal, pauseWorldDrawing ? 1 : 0, &load),
              GE_Result_Ok);
    return load;
}

bool ScriptIsLoading()
{
    uint8_t loading = 0;
    EXPECT_EQ(Table().IsLoading(&loading), GE_Result_Ok);
    return loading != 0;
}

bool ScriptIsReadyToShow(ScriptOperation load)
{
    uint8_t ready = 0;
    EXPECT_EQ(Table().IsReadyToShow(&load, &ready), GE_Result_Ok);
    return ready != 0;
}

std::string LastScriptError()
{
    char text[512] = {};
    int32_t length = 0;
    EXPECT_EQ(GE_GetLastScriptError(text, static_cast<int32_t>(sizeof(text)), &length), GE_Result_Ok);
    return std::string(text, static_cast<std::size_t>(std::min<int32_t>(length, sizeof(text))));
}

void WriteScene(const fs::path& root, const char* name)
{
    const fs::path path = root / "Assets/Scenes" / (std::string(name) + ".scene");
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "[scene name=\"" << name << "\" version=1]\n[entity id=\"e\"]\nName.value = \"" << name << "\"\n";
}

class SceneLoaderScriptApiTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_PreviousCwd = fs::current_path();
        m_Root = fs::temp_directory_path() / ("scene-loader-script-" + GUID::Generate().ToString());
        for (const char* name : {"A", "B", "C"})
            WriteScene(m_Root, name);
        std::ofstream camera(m_Root / "Assets/Scenes/Camera.scene", std::ios::binary | std::ios::trunc);
        camera << "[scene name=\"Camera\" version=1]\n[entity id=\"cam\"]\nName.value = \"Camera\"\n"
                  "Camera.worldDrawingPaused = false\n";
        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "SceneLoaderScriptApiTest";
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        config.Headless = true;
        m_Application = std::make_unique<ScriptLoadApplication>(config);
        ASSERT_TRUE(m_Application->Initialize());
        m_Host = std::make_unique<RuntimeHost>(*m_Application, RuntimeHostDesc{}, RuntimeHostHooks{});
        ASSERT_TRUE(m_Host->InitWindow());
        ASSERT_TRUE(m_Host->InitRendering());
    }

    void TearDown() override
    {
        if (m_Host)
            m_Host->Shutdown();
        m_Host.reset();
        if (m_Application)
            m_Application->Shutdown();
        m_Application.reset();
        std::error_code ec;
        fs::current_path(m_PreviousCwd, ec);
        fs::remove_all(m_Root, ec);
    }

    // Runs frames until `done`, at most kFrameCap of them. False at the cap.
    bool TickUntil(const std::function<bool()>& done)
    {
        for (int frame = 0; frame < kFrameCap && !done(); ++frame)
            m_Application->Tick();
        return done();
    }

    static bool Holds(std::string_view name)
    {
        ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
        bool found = false;
        if (world)
            world->Query<ECS::Read<Components::Name>>().Each(
                [&](const Components::Name& entityName) { found = found || entityName.View() == name; });
        return found;
    }

    // The WorldDrawingPaused of the scene's camera, or nullopt while the world has no camera.
    static std::optional<bool> CameraPaused()
    {
        std::optional<bool> paused;
        if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
            world->Query<ECS::Read<Components::Camera>>().Each(
                [&](const Components::Camera& camera) { paused = camera.WorldDrawingPaused; });
        return paused;
    }

    fs::path m_PreviousCwd;
    fs::path m_Root;
    std::unique_ptr<ScriptLoadApplication> m_Application;
    std::unique_ptr<RuntimeHost> m_Host;
};
} // namespace

TEST_F(SceneLoaderScriptApiTest, AScriptPollsALoadToReadyAndRevealsItWhenTheGameReleasesIt)
{
    const ScriptOperation load = ScriptLoad("Scenes/A.scene", ScriptRevealPolicy::WhenGameReleases);
    ASSERT_NE(load.Id, 0u);
    EXPECT_TRUE(ScriptIsLoading());
    ASSERT_TRUE(TickUntil([&] { return ScriptIsReadyToShow(load); }));

    float progress = 0.0f;
    double secondsRemaining = -1.0;
    EXPECT_EQ(Table().Progress(&load, &progress), GE_Result_Ok);
    EXPECT_EQ(Table().EstimatedSecondsRemaining(&load, &secondsRemaining), GE_Result_Ok);
    EXPECT_EQ(progress, 1.0f);
    EXPECT_EQ(secondsRemaining, 0.0);
    for (int frame = 0; frame < 3; ++frame)
        m_Application->Tick();
    EXPECT_TRUE(ScriptIsLoading()) << "the load waits for the game's Reveal";

    EXPECT_EQ(Table().Reveal(&load), GE_Result_Ok);
    m_Application->Tick();
    EXPECT_FALSE(ScriptIsLoading());
    EXPECT_TRUE(Holds("A"));
    EXPECT_TRUE(ScriptIsReadyToShow(load)) << "the operation stays readable after its load ends";
}

TEST_F(SceneLoaderScriptApiTest, AScriptLoadThatPausesWorldDrawingPausesTheCamerasUntilTheWorldDrawsAgain)
{
    const ScriptOperation load = ScriptLoad("Scenes/Camera.scene", ScriptRevealPolicy::AtReady, true);
    std::optional<bool> pausedOnceBuilt;
    ASSERT_TRUE(TickUntil(
        [&]
        {
            if (!pausedOnceBuilt)
                pausedOnceBuilt = CameraPaused();
            return ScriptIsReadyToShow(load);
        }));
    EXPECT_EQ(pausedOnceBuilt, true) << "the load pauses the new scene's camera";
    EXPECT_EQ(CameraPaused(), false) << "the load resumes the world before it is ready";
}

TEST_F(SceneLoaderScriptApiTest, ANewerScriptLoadRefusesTheOldOperationAndANativeLoadLeavesItReadable)
{
    const ScriptOperation first = ScriptLoad("Scenes/A.scene", ScriptRevealPolicy::AtReady);
    const ScriptOperation second = ScriptLoad("Scenes/B.scene", ScriptRevealPolicy::AtReady);
    EXPECT_GT(second.Id, first.Id);

    float progress = 0.0f;
    EXPECT_EQ(Table().Progress(&first, &progress), GE_Result_Fail);
    EXPECT_NE(LastScriptError().find("replaced by load"), std::string::npos) << LastScriptError();
    EXPECT_EQ(Table().Reveal(&first), GE_Result_Fail);
    EXPECT_EQ(Table().Progress(&second, &progress), GE_Result_Ok);

    EngineCore::GetInstance().GetSceneLoader().Load({.Scene = "Scenes/C.scene"});
    ASSERT_TRUE(TickUntil([] { return Holds("C"); }));
    EXPECT_FALSE(ScriptIsReadyToShow(second)) << "the native load cancelled it";
    EXPECT_EQ(Table().Reveal(&second), GE_Result_Ok);
}
