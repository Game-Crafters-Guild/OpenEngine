// RuntimeHost's frame contract, on a real window and device: one rendered frame calls the
// composite hook once, and a readback the hook declares on the composited image resolves
// after the frame, at the window's framebuffer size. The host stamps the frame's readbacks
// after Execute: a stamp taken before it carries the first frame's empty token, which never
// reports completion, so the readback would not resolve. A framebuffer resize recreates the
// swapchain at the new size as the resize arrives, not at the next out-of-date acquire. A world
// with no camera gives one warning each time it loses its camera, not one each frame.
//
// The host opens a visible 64 x 64 window (RuntimeHostDesc has no hidden mode). It stays open
// for the fixture's whole run, a frame or two plus the engine bring-up and shutdown: about 2 s
// with warm caches, about 30 s cold.
// Skips when no window or Vulkan device can be created.
//
// A headless application brings the same host up with no window and no device: the scene
// opens, the engine's schedule and a native script run every fixed frame in real time, and the
// script's quit ends the run. It needs neither, so it never skips. A game's save of the
// headless world through a save slot keeps its AI: the load continues the simulation tick, and the
// agents then decide as the agents of the world that was not saved.

#include "AI/RecordStore.h"
#include "AIECS/AiGroupKnowledge.h"
#include "AIECS/AiRegistry.h"
#include "AIECS/AiTaskContext.h"
#include "AIECS/AiTestWorld.h"
#include "AIECS/Assets/AiOptionSetData.h"
#include "AIECS/Components/AiAgent.h"
#include "AIECS/Components/AiCells/AiCellsSmall.h"
#include "AIECS/Components/AiClock.h"
#include "AIECS/Components/AiGroup.h"
#include "AIECS/Components/AiTrace.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Components/Name.h"
#include "Core/Application.h"
#include "Core/ApplicationPaths.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/SimulationClock.h"
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
#include "EngineLogCapture.h"
#include "NativeScripting/UserSystemRegistry.h"
#include "Platform/Window.h"
#include "PlatformServices/PlatformServices.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"
#include "Scene/SceneIO.h"
#include "ScopedPlatformCacheRoot.h"
#include "Scripting/ScriptsConfig.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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

// The missing-camera warnings among the captured lines, once the log dispatch has caught up.
std::ptrdiff_t MissingCameraWarnings(const std::vector<std::string>& lines)
{
    Logger::Log::Flush();
    return std::count_if(lines.begin(), lines.end(),
                         [](const std::string& line) { return line.find("no enabled Camera") != std::string::npos; });
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
        // A pipeline with no passes presents the host's own colour target; the empty primary
        // world gets the host's default camera, so the frame has a view.
        WriteFile(m_Root / "Assets/RenderPipelines/HostTest.rendergraph",
                  R"({"schemaVersion":2,"pipelineName":"HostTest","passes":[],"outputs":{"FinalColor":"View.Color"}})");

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

    // The empty primary world with the host's default camera. False when there is no world.
    bool PrepareWorld()
    {
        ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
        if (!world)
            return false;
        m_Host->EnsureCamera(*world);
        return true;
    }

    // Render one frame and return the face the game UI uses for an unstyled element.
    std::optional<Rendering::Text::FaceDebugInfo> RenderAndGetDefaultHudFace()
    {
        if (m_Host->LoadRenderPipeline() != Engine::Renderer::PipelineResolveFailure::None || !PrepareWorld())
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
    ASSERT_TRUE(PrepareWorld());

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

TEST_F(RuntimeHostFrame, AWorldWithNoCameraWarnsOnceEachTimeItLosesItsCamera)
{
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);

    for (int frame = 0; frame < 3; ++frame)
        m_Host->Render(1.0f / 60.0f);
    EXPECT_EQ(MissingCameraWarnings(lines), 1) << "frames with no camera give one warning, not one each frame";

    m_Host->EnsureCamera(*world);
    m_Host->Render(1.0f / 60.0f);
    world->Clear();
    for (int frame = 0; frame < 2; ++frame)
        m_Host->Render(1.0f / 60.0f);
    m_Host->GetRenderDeviceContext()->GetDevice()->WaitForIdle();
    EXPECT_EQ(MissingCameraWarnings(lines), 2) << "a camera lost again gives one more warning";
    EXPECT_EQ(m_DeviceFailures, 0);
}

TEST_F(RuntimeHostFrame, AFramebufferResizeRecreatesTheSwapchainAtTheNewSize)
{
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    ASSERT_TRUE(PrepareWorld());
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
        fs::create_directories(m_Root / "Assets");

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

    Engine::Renderer::RenderingLoop* loop = EngineCore::GetInstance().GetRenderingLoop();
    ASSERT_NE(loop, nullptr) << "the engine's schedule runs without render services";
    FrameCountingSystem* system = loop->GetSystemManager()->AddSystem<FrameCountingSystem>();
    QuittingScript script;
    ASSERT_TRUE(NativeScripting::RegisterUserSystem(&script));
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
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
    EXPECT_EQ(world->GetSimulationClock().GetTick(), static_cast<uint64>(kHeadlessFrames))
        << "a headless frame at the step rate runs one simulation step";
    EXPECT_EQ(world->GetSimulationClock().GetAlpha(), 0.0f) << "a headless frame leaves no part of a step to blend";
    EXPECT_DOUBLE_EQ(m_Application->GetDeltaTime(), 1.0 / kHeadlessFps);
    EXPECT_GE(seconds, (kHeadlessFrames - 1) / kHeadlessFps)
        << "a headless fixed step is paced to real time, not run as fast as the CPU allows";
}

namespace
{
constexpr uint64 kAiSeed = 11;
constexpr uint16 kTargetCell = 2;
constexpr uint16 kCooldownCell = 4;
constexpr float32 kHuntScore = 0.45f;
constexpr int kSavedFrames = 30;
constexpr int kComparedFrames = 36;
constexpr std::string_view kSaveSlot = "slot-1";
constexpr std::string_view kSaveFile = "world.scene";
constexpr std::array<std::string_view, 2> kAgentNames{"Hunter", "Watcher"};

// The sensor sees these subjects, owned by g_Owner, while g_Seeing is true.
bool g_Seeing = false;
std::array<ECS::EntityHandle, 2> g_Subjects{};
ECS::EntityHandle g_Owner;

void SeeSubjects(ECS::World&, ECS::EntityHandle, AI::RecordStore& records, uint32 tick)
{
    if (!g_Seeing)
        return;
    for (const ECS::EntityHandle subject : g_Subjects)
    {
        AI::RecordStore::Sighting sighting;
        sighting.Subject = subject.id;
        sighting.Owner = g_Owner.id;
        sighting.Strength = 0.5f;
        records.Observe(sighting, tick);
    }
}

// kHuntScore when the entity in the agent's target cell is the entity named "Target", else 0.
void ReadTarget(const ECS::World& world, std::span<const ECS::EntityHandle> agents, std::span<float32> out)
{
    for (size_t index = 0; index < agents.size(); ++index)
    {
        const Components::AiCellsSmall* cells = world.GetComponent<Components::AiCellsSmall>(agents[index]);
        const ECS::EntityHandle target(cells->Values[kTargetCell]);
        const Components::Name* name = target.IsValid() ? world.GetComponent<Components::Name>(target) : nullptr;
        out[index] = name != nullptr && name->View() == "Target" ? kHuntScore : 0.0f;
    }
}

AIECS::AiTaskStatus TickHunt(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Running;
}

AIECS::AiTaskStatus TickWait(AIECS::AiTaskContext&)
{
    return AIECS::AiTaskStatus::Succeeded;
}

// Option 0 hunts with the target score; option 1 waits at a flat 0.5 and then cools down for 10
// AI ticks. The program has a stable id, so a save can name it.
uint16 CompileHuntProgram()
{
    AIECS::AiRegistry& registry = AIECS::AiRegistry::Get();
    registry.RegisterInput(&ReadTarget, AIECS::AiInputThreading::CallingThread, "RuntimeHostSave.Target");
    registry.RegisterTask({.Tick = &TickHunt, .Name = "RuntimeHostSave.Hunt"});
    registry.RegisterTask({.Tick = &TickWait, .Name = "RuntimeHostSave.Wait"});
    registry.RegisterSensor(&SeeSubjects);
    AIECS::AiOptionSetData data;
    data.InputNames = {"RuntimeHostSave.Target"};
    data.TaskNames = {"RuntimeHostSave.Hunt", "RuntimeHostSave.Wait"};
    data.Options.Curves.push_back(AI::ResponseCurve::FromPreset(AI::CurvePreset::Linear, 1.0f, 0.0f));
    data.Options.Considerations.push_back({0.0f, 1.0f, 0, 0});
    data.Options.Options.push_back({.Weight = 1.0f, .ConsiderationCount = 1, .Action = 0});
    data.Options.Options.push_back({.Weight = 0.5f, .Action = 1, .CooldownTicks = 10});
    data.CooldownCell = kCooldownCell;
    data.EntityCells = {kTargetCell};
    data.StableId = GUID::Generate();
    return registry.CompileProgram(data);
}

ECS::EntityHandle CreateNamed(ECS::World& world, std::string_view name)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::Name component{};
    name.copy(component.value, sizeof(component.value) - 1);
    world.AddComponentImmediate(entity, component);
    return entity;
}

ECS::EntityHandle FindNamed(ECS::World& world, std::string_view name)
{
    ECS::EntityHandle found;
    world.Query<ECS::Read<Components::Name>>().Each(
        [&found, name](ECS::EntityHandle entity, const Components::Name& entityName)
        {
            if (entityName.View() == name)
                found = entity;
        });
    return found;
}

void SetAiSeed(ECS::World& world)
{
    Components::AiClock clock;
    clock.Seed = kAiSeed;
    world.SetSingleton(clock);
}

// The game's save: the scene with the simulation tick, written into a save slot and committed.
void SaveToSlot(ECS::World& world, const fs::path& scratch)
{
    Scene::SceneWriteOptions options;
    options.writeSimulationTick = true;
    ASSERT_TRUE(Scene::WriteSceneFile(world, scratch, options));
    const std::vector<uint8_t> bytes = Rendering::Utils::ReadFile(scratch.string());
    fs::remove(scratch);
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(PlatformServices::Get().WriteFile(kSaveSlot, kSaveFile, bytes), PlatformServices::Result::Ok);
    ASSERT_EQ(PlatformServices::Get().Commit(kSaveSlot), PlatformServices::Result::Ok);
}

// The game's load: the scene read from the save slot into the world, which the load clears.
void LoadFromSlot(ECS::World& world, const fs::path& scratch)
{
    std::vector<uint8_t> bytes;
    ASSERT_EQ(PlatformServices::Get().ReadFile(kSaveSlot, kSaveFile, bytes), PlatformServices::Result::Ok);
    ASSERT_TRUE(Rendering::Utils::WriteFile(scratch.string(), bytes));
    const Scene::SceneInstantiateResult result = Scene::ReadSceneInto(world, scratch);
    ASSERT_TRUE(result) << result.Error->message;
}

struct SavedRecords
{
    std::vector<uint32> Subjects;
    std::vector<uint32> Owners;
    std::vector<uint32> FirstSeen;
    std::vector<uint32> LastSeen;
};

SavedRecords CopyRecords(const AI::RecordStore& records)
{
    return {{records.GetSubjects().begin(), records.GetSubjects().end()},
            {records.GetOwners().begin(), records.GetOwners().end()},
            {records.GetFirstSeen().begin(), records.GetFirstSeen().end()},
            {records.GetLastSeen().begin(), records.GetLastSeen().end()}};
}

class HeadlessRuntimeHostSave : public HeadlessRuntimeHost
{
protected:
    void SetUp() override
    {
        HeadlessRuntimeHost::SetUp();
        if (HasFatalFailure())
            return;
        PlatformServices::Get().Shutdown();
        PlatformServices::Config config;
        config.AppName = "RuntimeHostHeadlessTest";
        config.SaveRootOverride = m_Root / "Saves";
        ASSERT_EQ(PlatformServices::Get().Initialize(config), PlatformServices::Result::Ok);
    }

    void TearDown() override
    {
        PlatformServices::Get().Shutdown();
        HeadlessRuntimeHost::TearDown();
    }

    void RunFrames(int count)
    {
        for (int frame = 0; frame < count; ++frame)
            ASSERT_TRUE(m_Application->Tick());
    }
};
} // namespace

// The save and the load of a game on the Player's headless host: the simulation tick continues
// from the save, the agents and the group's records come back with their entity references on the
// loaded entities, and the loaded agents decide as the agents of the world that was not saved.
TEST_F(HeadlessRuntimeHostSave, AGameSaveInASlotKeepsTheAiAndItContinuesAfterTheLoad)
{
    ASSERT_TRUE(m_Host->InitWindow());
    ASSERT_TRUE(m_Host->InitRendering());
    ASSERT_EQ(m_Host->LoadRenderPipeline(), Engine::Renderer::PipelineResolveFailure::None);
    const uint16 program = CompileHuntProgram();
    ASSERT_NE(program, Components::AiAgent::kNoProgram);
    ECS::World& world = *EngineCore::GetInstance().EnsurePrimaryWorld();
    m_Application->SetFixedFrameRate(kHeadlessFps);
    SetAiSeed(world);

    // These entities go away, so the saved entities have ids that the load does not give them.
    for (int filler = 0; filler < 5; ++filler)
        world.DestroyEntityImmediate(world.CreateEntity());
    const ECS::EntityHandle target = CreateNamed(world, "Target");
    const ECS::EntityHandle other = CreateNamed(world, "Other");
    const std::array<ECS::EntityHandle, 2> cellTargets{target, other};
    std::array<ECS::EntityHandle, 2> agents{};
    for (size_t index = 0; index < agents.size(); ++index)
    {
        agents[index] = CreateNamed(world, kAgentNames[index]);
        world.AddComponentImmediate(agents[index], Components::AiAgent{.Program = program});
        Components::AiCellsSmall cells;
        cells.Values[kTargetCell] = cellTargets[index].id;
        world.AddComponentImmediate(agents[index], cells);
    }
    const ECS::EntityHandle group = CreateNamed(world, "Group");
    Components::AiGroup groupSettings;
    groupSettings.IdsAreEntities = true;
    world.AddComponentImmediate(group, groupSettings);

    g_Subjects = {other, target};
    g_Owner = target;
    g_Seeing = true;
    RunFrames(kSavedFrames);
    g_Seeing = false;
    ASSERT_FALSE(HasFatalFailure());

    const uint64 savedTick = world.GetSimulationClock().GetTick();
    std::array<Components::AiAgent, 2> savedAgents{};
    std::array<Components::AiCellsSmall, 2> savedCells{};
    for (size_t index = 0; index < agents.size(); ++index)
    {
        savedAgents[index] = *world.GetComponent<Components::AiAgent>(agents[index]);
        savedCells[index] = *world.GetComponent<Components::AiCellsSmall>(agents[index]);
    }
    for (const Components::AiCellsSmall& cells : savedCells)
        ASSERT_TRUE(std::any_of(cells.Values.begin() + kCooldownCell, cells.Values.end(), [](uint32 value) { return value != 0; }))
            << "the fixture saves agents with a cooldown that runs";
    const AI::RecordStore* records = AIECS::FindGroupRecords(world, group);
    ASSERT_NE(records, nullptr);
    ASSERT_EQ(records->GetCount(), 2u);
    const SavedRecords savedRecords = CopyRecords(*records);
    SaveToSlot(world, m_Root / "save-scratch.scene");
    ASSERT_FALSE(HasFatalFailure());

    // The world that is not saved continues; its traces are the oracle for the loaded world.
    for (const ECS::EntityHandle agent : agents)
        world.AddComponentImmediate(agent, Components::AiTrace{});
    RunFrames(kComparedFrames);
    std::array<Components::AiTrace, 2> unsavedTraces{};
    for (size_t index = 0; index < agents.size(); ++index)
        unsavedTraces[index] = *world.GetComponent<Components::AiTrace>(agents[index]);
    const uint64 unsavedEndTick = world.GetSimulationClock().GetTick();

    LoadFromSlot(world, m_Root / "load-scratch.scene");
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(world.GetSimulationClock().GetTick(), savedTick) << "the load continues the saved tick";
    SetAiSeed(world);

    const ECS::EntityHandle loadedTarget = FindNamed(world, "Target");
    const ECS::EntityHandle loadedOther = FindNamed(world, "Other");
    const ECS::EntityHandle loadedGroup = FindNamed(world, "Group");
    ASSERT_TRUE(loadedTarget.IsValid() && loadedOther.IsValid() && loadedGroup.IsValid());
    ASSERT_NE(loadedTarget.id, target.id) << "the fixture needs ids that change in the load";
    const std::array<ECS::EntityHandle, 2> loadedCellTargets{loadedTarget, loadedOther};
    std::array<ECS::EntityHandle, 2> loadedAgents{};
    for (size_t index = 0; index < loadedAgents.size(); ++index)
    {
        loadedAgents[index] = FindNamed(world, kAgentNames[index]);
        ASSERT_TRUE(loadedAgents[index].IsValid());
        const Components::AiAgent& agent = *world.GetComponent<Components::AiAgent>(loadedAgents[index]);
        EXPECT_EQ(agent.Program, program);
        EXPECT_EQ(agent.NextTick, savedAgents[index].NextTick) << kAgentNames[index];
        EXPECT_EQ(agent.EnteredTick, savedAgents[index].EnteredTick) << kAgentNames[index];
        EXPECT_EQ(agent.ActivePath[0], savedAgents[index].ActivePath[0]) << kAgentNames[index];
        const Components::AiCellsSmall& cells = *world.GetComponent<Components::AiCellsSmall>(loadedAgents[index]);
        EXPECT_EQ(cells.Values[kTargetCell], loadedCellTargets[index].id) << kAgentNames[index];
        for (uint32 cell = kCooldownCell; cell < Components::AiCellsSmall::kCount; ++cell)
            EXPECT_EQ(cells.Values[cell], savedCells[index].Values[cell]) << kAgentNames[index] << " cooldown cell " << cell;
    }

    const AI::RecordStore* loadedRecords = AIECS::FindGroupRecords(world, loadedGroup);
    ASSERT_NE(loadedRecords, nullptr);
    ASSERT_EQ(loadedRecords->GetCount(), 2u);
    const SavedRecords loaded = CopyRecords(*loadedRecords);
    EXPECT_EQ(loaded.Subjects, (std::vector<uint32>{loadedOther.id, loadedTarget.id}));
    EXPECT_EQ(loaded.Owners, (std::vector<uint32>{loadedTarget.id, loadedTarget.id}));
    EXPECT_EQ(loaded.FirstSeen, savedRecords.FirstSeen);
    EXPECT_EQ(loaded.LastSeen, savedRecords.LastSeen);

    for (const ECS::EntityHandle agent : loadedAgents)
        world.AddComponentImmediate(agent, Components::AiTrace{});
    RunFrames(kComparedFrames);
    EXPECT_EQ(world.GetSimulationClock().GetTick(), unsavedEndTick);
    for (size_t index = 0; index < loadedAgents.size(); ++index)
    {
        const Components::AiTrace& trace = *world.GetComponent<Components::AiTrace>(loadedAgents[index]);
        EXPECT_GT(unsavedTraces[index].Count, 0u) << kAgentNames[index];
        EXPECT_TRUE(AIECS::AiTestWorld::SameTrace(unsavedTraces[index], trace)) << kAgentNames[index];
    }
}
