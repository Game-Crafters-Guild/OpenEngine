// RuntimeHost's frame contract, on a real window and device: one rendered frame calls the
// composite hook once, and a readback the hook declares on the composited image resolves
// after the frame, at the window's framebuffer size. The host stamps the frame's readbacks
// after Execute: a stamp taken before it carries the first frame's empty token, which never
// reports completion, so the readback would not resolve. A framebuffer resize recreates the
// swapchain at the new size as the resize arrives, not at the next out-of-date acquire.
//
// The host opens a visible 64 x 64 window (RuntimeHostDesc has no hidden mode). It stays open
// for the fixture's whole run, a frame or two plus the engine bring-up and shutdown: about 2 s
// with warm caches, about 30 s cold.
// Skips when no window or Vulkan device can be created.
//
// A headless application brings the same host up with no window and no device: the scene
// opens, the engine's schedule and a native script run every fixed frame in real time, and the
// script's quit ends the run. It needs neither, so it never skips.

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Core/Application.h"
#include "Core/ApplicationPaths.h"
#include "Core/Engine.h"
#include "ECS/Systems.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Engine/Build/PackagedGameLayout.h"
#include "Engine/GameUI/GameUIHost.h"
#include "Engine/GameUI/GameplayUI.h"
#include "Engine/Hosting/RuntimeFrameReadback.h"
#include "Engine/Hosting/RuntimeHost.h"
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderDeviceContext.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "NativeScripting/UserSystemRegistry.h"
#include "Platform/Window.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"
#include "ScopedPlatformCacheRoot.h"
#include "Scripting/ScriptsConfig.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIManager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kWindowSize = 64;

fs::path ResolveBuildShader(const fs::path& path)
{
    const fs::path candidate = fs::path(ENGINE_TEST_BUILD_DIR) / path;
    std::error_code ec;
    return fs::is_regular_file(candidate, ec) ? candidate : fs::path{};
}

void WriteFile(const fs::path& path, const char* text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

class HostTestApplication final : public Application
{
public:
    using Application::Application;
};

// What the composite hook saw during the frame.
struct CompositeProbe
{
    int Calls = 0;
    std::shared_ptr<Rendering::RGReadbackTicket> Ticket;

    void OnFrameComposited(RuntimeFrameReadback& readback)
    {
        ++Calls;
        Ticket = Rendering::RequestTextureReadbackRG(&readback.GetDevice(), readback.GetFrame(),
                                                     readback.GetComposite().PresentSrc,
                                                     "RuntimeHostTests.Composite");
    }
};

// Brings up the engine with a project holding a pass-less pipeline and an empty scene, and a
// host over it. TearDown releases everything whether or not the test body returned early.
class RuntimeHostFrame : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_PreviousCwd = fs::current_path();
        m_Root = fs::temp_directory_path() / ("runtime-host-" + GUID::Generate().ToString());
        // The host's device persists its pipeline cache (RuntimeHost opts in for the Player), so
        // the platform cache root lives under m_Root for the test: the user's cache is neither
        // read nor written, and the test's file goes with m_Root.
        m_CacheRoot.emplace(m_Root / "cache-root");
        // A pipeline with no passes presents the host's own colour target; the empty scene gets
        // the host's default camera, so the frame has a view.
        WriteFile(m_Root / "Assets/RenderPipelines/HostTest.rendergraph",
                  R"({"schemaVersion":2,"pipelineName":"HostTest","passes":[],"outputs":{"FinalColor":"View.Color"}})");
        WriteFile(m_Root / "Assets/Scenes/Empty.scene", "[scene name=\"Empty\" version=1]\n");

        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "RuntimeHostTest";
        config.WindowWidth = kWindowSize;
        config.WindowHeight = kWindowSize;
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        m_Application = std::make_unique<HostTestApplication>(config);
        ASSERT_TRUE(m_Application->Initialize());
        Rendering::Utils::SetShaderFileLoader(nullptr);
        Rendering::Utils::SetShaderPathResolver(&ResolveBuildShader);
        auto& assetManager = EngineCore::GetInstance().GetAssetManager();
        assetManager.WaitForStartupScan(kAssetSourceAliasProject);
        // Mount the staged runtime assets as 'editor', as the Player does.
        const auto editorSource = BuildPlayerEditorSourceDesc(EngineCore::GetInstance().GetResolvedAssetRoot(),
                                                              ApplicationPaths::GetInstallAssetsRoot());
        ASSERT_TRUE(editorSource.has_value());
        ASSERT_TRUE(assetManager.RegisterSource(*editorSource));

        RuntimeHostDesc desc;
        desc.Title = "RuntimeHostTest";
        desc.WindowWidth = kWindowSize;
        desc.WindowHeight = kWindowSize;
        desc.VSync = false;
        desc.RenderPipeline = "RenderPipelines/HostTest.rendergraph";
        RuntimeHostHooks hooks;
        hooks.OnDeviceFailed = [this] { ++m_DeviceFailures; };
        hooks.OnFrameComposited = [this](RuntimeFrameReadback& readback) { m_Probe.OnFrameComposited(readback); };
        m_Host = std::make_unique<RuntimeHost>(*m_Application, desc, std::move(hooks));
        if (!m_Host->InitWindow() || !m_Host->InitRendering())
            GTEST_SKIP() << "no window or Vulkan device could be created";
    }

    // Render one frame and return the face the game UI uses for an unstyled element.
    std::optional<Rendering::Text::FaceDebugInfo> RenderAndGetDefaultHudFace()
    {
        if (m_Host->LoadRenderPipeline() != Engine::Renderer::PipelineResolveFailure::None ||
            !m_Host->OpenScene("Scenes/Empty.scene"))
            return std::nullopt;
        m_Host->Render(1.0f / 60.0f);
        m_Host->GetRenderDeviceContext()->GetDevice()->WaitForIdle();
        auto* gameUi = GameUI::GetHost();
        if (!gameUi)
            return std::nullopt;
        const auto* font = gameUi->GetUIManager()->ResolveFontForStyle(ResolvedStyle{});
        return font ? font->GetFaceDebugInfo() : std::nullopt;
    }

    void TearDown() override
    {
        m_Probe.Ticket.reset();
        if (m_Host)
            m_Host->Shutdown();
        m_Host.reset();
        if (m_Application)
            m_Application->Shutdown();
        m_Application.reset();
        m_CacheRoot.reset();
        std::error_code ec;
        fs::current_path(m_PreviousCwd, ec);
        fs::remove_all(m_Root, ec);
    }

    fs::path m_PreviousCwd;
    fs::path m_Root;
    std::optional<TestUtils::ScopedPlatformCacheRoot> m_CacheRoot;
    std::unique_ptr<HostTestApplication> m_Application;
    std::unique_ptr<RuntimeHost> m_Host;
    CompositeProbe m_Probe;
    int m_DeviceFailures = 0;
};
} // namespace

TEST_F(RuntimeHostFrame, DefaultHudFontComesFromStagedAssetsWithASeparateProject)
{
    ASSERT_FALSE(fs::exists(m_Root / "Assets/Fonts/Roboto-Regular.ttf"));
    ASSERT_TRUE(fs::exists(ApplicationPaths::GetInstallAssetsRoot() / "Fonts/Roboto-Regular.ttf"))
        << "the runtime font must be staged beside the test executable";
    const auto face = RenderAndGetDefaultHudFace();
    ASSERT_TRUE(face.has_value()) << "a project without its own font must use the runtime's bundled default";
    EXPECT_EQ(face->family, "Roboto");
    EXPECT_EQ(face->style, "Regular");
}

TEST_F(RuntimeHostFrame, DefaultHudFontComesFromTheProjectWhenTheProjectHasOne)
{
    // The project's Fonts/Roboto-Regular.ttf holds a different face, so the test can tell the copies apart.
    std::error_code ec;
    fs::create_directories(m_Root / "Assets/Fonts", ec);
    ASSERT_TRUE(fs::copy_file(ApplicationPaths::GetInstallAssetsRoot() / "Fonts/RobotoMono-Regular.ttf",
                              m_Root / "Assets/Fonts/Roboto-Regular.ttf", ec))
        << ec.message();
    const auto face = RenderAndGetDefaultHudFace();
    ASSERT_TRUE(face.has_value());
    EXPECT_EQ(face->family, "Roboto Mono");
}

TEST_F(RuntimeHostFrame, OneFrameCallsTheCompositeHookOnceAndItsReadbackResolves)
{
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ASSERT_TRUE(m_Host->OpenScene("Scenes/Empty.scene"));

    m_Host->Render(1.0f / 60.0f);
    m_Host->GetRenderDeviceContext()->GetDevice()->WaitForIdle();

    EXPECT_EQ(m_Probe.Calls, 1) << "one rendered frame calls the composite hook once";
    EXPECT_EQ(m_DeviceFailures, 0);
    EXPECT_FALSE(m_Host->GetRenderDeviceContext()->GetDevice()->IsVsyncEnabled())
        << "the device takes the descriptor's vsync";
    ASSERT_TRUE(m_Probe.Ticket) << "the hook's readback of the composited image was not declared";
    Rendering::ViewReadbackResult result{};
    ASSERT_TRUE(m_Probe.Ticket->TryGet(result))
        << "a readback declared in the hook resolves once the frame finished: the host stamps "
           "the frame's readbacks after Execute";
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    m_Host->GetWindow()->GetFramebufferSize(framebufferWidth, framebufferHeight);
    EXPECT_EQ(result.width, static_cast<uint32_t>(framebufferWidth));
    EXPECT_EQ(result.height, static_cast<uint32_t>(framebufferHeight));
}

TEST_F(RuntimeHostFrame, AFramebufferResizeRecreatesTheSwapchainAtTheNewSize)
{
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ASSERT_TRUE(m_Host->OpenScene("Scenes/Empty.scene"));
    m_Host->Render(1.0f / 60.0f);
    Rendering::IDevice& device = *m_Host->GetRenderDeviceContext()->GetDevice();
    uint32_t initialWidth = 0;
    uint32_t initialHeight = 0;
    ASSERT_TRUE(device.GetSwapchainSize(initialWidth, initialHeight));

    // Larger than any minimum window size the platform imposes on the 64 x 64 request.
    constexpr int kResizedWidth = 320;
    constexpr int kResizedHeight = 240;
    m_Host->GetWindow()->SetWindowSize(kResizedWidth, kResizedHeight);
    // Win32 resizes synchronously; X11 round-trips through the window manager, so the events
    // are pumped until the framebuffer changes, for at most kResizeWaitPolls x 10 ms (2 s).
    constexpr int kResizeWaitPolls = 200;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    for (int poll = 0; poll < kResizeWaitPolls; ++poll)
    {
        Platform::Window::PollEvents();
        m_Host->GetWindow()->GetFramebufferSize(framebufferWidth, framebufferHeight);
        if (static_cast<uint32_t>(framebufferWidth) != initialWidth &&
            static_cast<uint32_t>(framebufferHeight) != initialHeight)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(static_cast<uint32_t>(framebufferWidth), initialWidth) << "the window did not resize";
    ASSERT_NE(static_cast<uint32_t>(framebufferHeight), initialHeight) << "the window did not resize";

    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    ASSERT_TRUE(device.GetSwapchainSize(swapchainWidth, swapchainHeight));
    EXPECT_EQ(swapchainWidth, static_cast<uint32_t>(framebufferWidth))
        << "the swapchain follows the framebuffer when it changes, before any acquire: a browser "
           "canvas never reports the resize as out of date";
    EXPECT_EQ(swapchainHeight, static_cast<uint32_t>(framebufferHeight));

    m_Probe.Calls = 0;
    m_Host->Render(1.0f / 60.0f);
    device.WaitForIdle();
    EXPECT_EQ(m_Probe.Calls, 1) << "the first frame after the resize renders";
    EXPECT_EQ(m_DeviceFailures, 0);
}

namespace
{
constexpr int kHeadlessFrames = 30;
constexpr double kHeadlessFps = 60.0;

// An ECS system on the engine's schedule, as a package or the managed bridge adds one.
class FrameCountingSystem final : public ECS::ISystem
{
public:
    int Updates = 0;
    void Update(ECS::World&, float32) override { ++Updates; }
    const char* GetName() const override { return "RuntimeHostTests.FrameCounting"; }
};

// A native user script: counts its update and post-simulation hooks, and on its
// kHeadlessFrames-th update asks the application to quit, as a game's script would.
class QuittingScript final : public NativeScripting::IUserSystem
{
public:
    int Updates = 0;
    int PostSimulations = 0;
    void OnStart(ECS::World&) override {}
    void OnUpdate(ECS::World&, float) override
    {
        if (++Updates == kHeadlessFrames)
            Application::Get()->RequestExit();
    }
    void OnDestroy(ECS::World&) override {}
    const char* Name() const override { return "RuntimeHostTests.QuittingScript"; }
    void OnPostSimulation(ECS::World&, float) override { ++PostSimulations; }
    bool HasPostSimulation() const override { return true; }
};

// The Player's frame: user scripts tick in Update, the host renders in Render.
class HeadlessTestApplication final : public Application
{
public:
    using Application::Application;
    RuntimeHost* Host = nullptr;

protected:
    void Update(float64 deltaTime) override
    {
        if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
            NativeScripting::TickUserSystems(*world, static_cast<float>(deltaTime));
    }
    void Render() override { Host->Render(static_cast<float>(GetDeltaTime())); }
};

class HeadlessRuntimeHost : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_PreviousCwd = fs::current_path();
        m_Root = fs::temp_directory_path() / ("runtime-host-headless-" + GUID::Generate().ToString());
        WriteFile(m_Root / "Assets/Scenes/Empty.scene", "[scene name=\"Empty\" version=1]\n");

        ScriptsConfig scripts{};
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        EngineCore::GetInstance().SetScriptsConfig(scripts);
        ApplicationConfig config{};
        config.Name = "RuntimeHostHeadlessTest";
        config.WorkspaceDirectory = m_Root.string();
        config.AssetDirectory = "Assets";
        config.Headless = true;
        m_Application = std::make_unique<HeadlessTestApplication>(config);
        ASSERT_TRUE(m_Application->Initialize());
        EngineCore::GetInstance().GetAssetManager().WaitForStartupScan(kAssetSourceAliasProject);

        RuntimeHostDesc desc;
        desc.Title = "RuntimeHostHeadlessTest";
        m_Host = std::make_unique<RuntimeHost>(*m_Application, desc, RuntimeHostHooks{});
        m_Application->Host = m_Host.get();
    }

    void TearDown() override
    {
        if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
            NativeScripting::StopUserSystems(*world);
        NativeScripting::ClearUserSystems();
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

    fs::path m_PreviousCwd;
    fs::path m_Root;
    std::unique_ptr<HeadlessTestApplication> m_Application;
    std::unique_ptr<RuntimeHost> m_Host;
};
} // namespace

TEST_F(HeadlessRuntimeHost, RunsTheScheduleAndAScriptOnAFixedStepWithNoWindowOrDeviceUntilTheScriptQuits)
{
    ASSERT_EQ(Application::Get(), m_Application.get());
    ASSERT_TRUE(m_Host->InitWindow());
    EXPECT_EQ(m_Host->GetWindow(), nullptr) << "a headless host opens no window";
    ASSERT_TRUE(m_Host->InitRendering());
    EXPECT_EQ(m_Host->GetRenderDeviceContext(), nullptr) << "a headless host creates no render device";
    EXPECT_EQ(EngineCore::GetInstance().GetAudioSystem(), nullptr) << "a headless engine opens no audio device";
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ASSERT_TRUE(m_Host->OpenScene("Scenes/Empty.scene"));

    Engine::Renderer::RenderingLoop* loop = EngineCore::GetInstance().GetRenderingLoop();
    ASSERT_NE(loop, nullptr) << "the engine's schedule runs without render services";
    FrameCountingSystem* system = loop->GetSystemManager()->AddSystem<FrameCountingSystem>();
    QuittingScript script;
    ASSERT_TRUE(NativeScripting::RegisterUserSystem(&script));
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    ASSERT_NE(world, nullptr);
    NativeScripting::StartUserSystems(*world);

    m_Application->SetFixedFrameRate(kHeadlessFps);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(m_Application->Run(), 0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(m_Application->GetFrameCount(), static_cast<uint64>(kHeadlessFrames))
        << "the script's quit ends the run after the frame it was asked in";
    EXPECT_EQ(script.Updates, kHeadlessFrames);
    EXPECT_EQ(script.PostSimulations, kHeadlessFrames) << "the schedule's native post-simulation hook runs every frame";
    EXPECT_EQ(system->Updates, kHeadlessFrames) << "the engine's schedule ticks every frame";
    EXPECT_DOUBLE_EQ(m_Application->GetDeltaTime(), 1.0 / kHeadlessFps);
    EXPECT_GE(seconds, (kHeadlessFrames - 1) / kHeadlessFps)
        << "a headless fixed step is paced to real time, not run as fast as the CPU allows";
}
