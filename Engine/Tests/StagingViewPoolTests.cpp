// The staging view and its pool: pooled entries, the view object's ownership, the on-demand arm, and a
// spawned cube rendered into the output.

#include "Components/Rendering/MeshRenderer.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/StagingViewPool.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Mathematics/HalfFloat.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Passes/TonemapPass.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "ScopedPipelineFrame.h"
#include "StagedTestPaths.h"
#include "SharedHeadlessDevice.h"
#include "WorldOnlyPipeline.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace
{
using namespace GameEngine;
using namespace GameEngine::Rendering;
using Engine::Renderer::RenderExtractionSystem;
using Engine::Renderer::RenderServices;
using Engine::Renderer::StagingViewDesc;
using Engine::Renderer::StagingView;
using Engine::Renderer::StagingViewPool;
using Engine::Renderer::TransformHierarchySystem;

constexpr uint32_t kSize = 32;

float HalfAt(const ViewReadbackResult& image, uint32_t x, uint32_t y, uint32_t channel)
{
    uint16_t bits = 0;
    std::memcpy(&bits, image.pixels.data() + ((y * image.width + x) * 4 + channel) * sizeof(uint16_t), sizeof(bits));
    return Mathematics::HalfToFloat(bits);
}

TEST(StagingViewPoolTests, AReleasedEntryIsReusedWithAnEmptyWorld)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    {
        StagingViewPool pool(services);
        StagingView first = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.First"});
        StagingView second = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Second"});
        ASSERT_TRUE(first.IsAlive());
        ASSERT_TRUE(second.IsAlive());
        EXPECT_NE(first.ViewId(), second.ViewId());
        EXPECT_NE(first.UiTextureName(), second.UiTextureName());
        EXPECT_NE(first.World().GetWorldId(), second.World().GetWorldId());

        const ViewDesc* view = services.Views().FindViewDesc(first.ViewId());
        ASSERT_NE(view, nullptr);
        EXPECT_EQ(view->participation, ViewParticipation::OnDemand);
        EXPECT_EQ(view->worldId, first.World().GetWorldId());
        EXPECT_FALSE(view->Participates()) << "an acquired view sleeps until its owner requests a frame";

        const ViewId firstView = first.ViewId();
        const ECS::EntityHandle spawned = first.World().CreateEntity();
        first = StagingView{};
        EXPECT_FALSE(first.IsAlive()) << "a view assigned an empty view holds no entry";
        StagingView reused = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Reused"});
        EXPECT_EQ(reused.ViewId(), firstView) << "the pool keeps the released entry's view";
        EXPECT_FALSE(reused.World().IsValid(spawned)) << "a reused entry starts with an empty world";
    }
    services.Shutdown();
}

// The destructor is the release: the entry goes back to the pool, and the next Acquire takes it.
TEST(StagingViewPoolTests, AViewReleasesItsEntryWhenItIsDestroyed)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    {
        StagingViewPool pool(services);
        ViewId destroyedView = 0;
        ECS::EntityHandle spawned{};
        {
            StagingView view = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Scoped"});
            destroyedView = view.ViewId();
            spawned = view.World().CreateEntity();
        }
        StagingView next = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Next"});
        EXPECT_EQ(next.ViewId(), destroyedView) << "a destroyed view returns its entry to the pool";
        EXPECT_FALSE(next.World().IsValid(spawned)) << "the release destroys the entities the host left";
    }
    services.Shutdown();
}

TEST(StagingViewPoolTests, AMovedViewKeepsOneOwner)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    {
        StagingViewPool pool(services);
        StagingView source = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Source"});
        const ViewId sourceView = source.ViewId();
        StagingView moved = std::move(source);
        EXPECT_FALSE(source.IsAlive()) << "the moved-from view holds no entry";
        ASSERT_TRUE(moved.IsAlive());
        EXPECT_EQ(moved.ViewId(), sourceView);

        StagingView target = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Target"});
        const ViewId targetView = target.ViewId();
        target = std::move(moved);
        EXPECT_FALSE(moved.IsAlive());
        EXPECT_EQ(target.ViewId(), sourceView) << "the assigned view takes the entry of the source";
        StagingView next = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Next"});
        EXPECT_EQ(next.ViewId(), targetView) << "the assignment releases the entry that the target held";

        target = StagingView{};
        StagingView last = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Last"});
        EXPECT_EQ(last.ViewId(), sourceView) << "the moved entry is released once, by its last owner";
    }
    services.Shutdown();
}

TEST(StagingViewPoolTests, AnEntryRetiresAtItsLastGenerationSoNoEntryRepeats)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    {
        // One release per handle generation: the entry retires before this count.
        constexpr uint32_t kMaxReleases = 256;
        StagingViewPool& pool = services.EnsureFeature<StagingViewPool>(services);
        const StagingViewDesc desc{kSize, kSize, "Staging.Retire"};
        StagingView view = pool.Acquire(desc);
        const std::string firstTexture = view.UiTextureName();
        const ViewId firstView = view.ViewId();
        uint32_t releases = 0;
        while (releases < kMaxReleases)
        {
            view = StagingView{};
            ++releases;
            if (!services.Views().FindViewDesc(firstView))
                break;
            view = pool.Acquire(desc);
            ASSERT_EQ(view.UiTextureName(), firstTexture) << "a released entry is reused until its last generation";
        }
        EXPECT_EQ(services.Views().FindViewDesc(firstView), nullptr) << "a retired entry frees its view";
        EXPECT_LT(releases, kMaxReleases);
        view = pool.Acquire(desc);
        EXPECT_NE(view.UiTextureName(), firstTexture) << "a retired entry is never reused";
    }
    services.Shutdown();
}

// RenderServices owns the pool as a render feature: one pool per services, destroyed at Shutdown
// while the view registry and the device live, so the views it still holds are freed.
TEST(StagingViewPoolTests, RenderServicesOwnsOnePoolAndFreesItsViewsAtShutdown)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    StagingViewPool& pool = services.EnsureFeature<StagingViewPool>(services);
    EXPECT_EQ(&services.EnsureFeature<StagingViewPool>(services), &pool) << "every host shares one pool";
    EXPECT_EQ(services.GetFeature<StagingViewPool>(), &pool);

    StagingView released = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Released"});
    StagingView acquired = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Held"});
    const ViewId releasedView = released.ViewId();
    const ViewId heldView = acquired.ViewId();
    released = StagingView{};
    ASSERT_NE(services.Views().FindViewDesc(releasedView), nullptr) << "a released entry keeps its sleeping view";
    ASSERT_NE(services.Views().FindViewDesc(heldView), nullptr);
    // The pool follows the move: its shutdown empties the object that holds the entry now.
    StagingView held = std::move(acquired);

    services.Shutdown();
    EXPECT_EQ(services.GetFeature<StagingViewPool>(), nullptr) << "Shutdown destroys the pool";
    EXPECT_EQ(services.Views().FindViewDesc(releasedView), nullptr) << "the pool frees a pooled view at shutdown";
    EXPECT_EQ(services.Views().FindViewDesc(heldView), nullptr) << "the pool frees a held view at shutdown";
    EXPECT_FALSE(held.IsAlive()) << "a view that outlives the renderer is empty, and its destructor does nothing";
}

TEST(StagingViewPoolTests, ARequestedFrameArmsTheViewForTheNextFrameOnly)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    {
        StagingViewPool pool(services);
        const StagingView view = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Arm"});
        services.Views().RequestViewFrame(view.ViewId());
        const auto participates = [&services, &view]
        { return services.Views().FindViewDesc(view.ViewId())->Participates(); };
        EXPECT_TRUE(participates());
        services.BeginWorldDrawFrame();
        EXPECT_TRUE(participates()) << "the request feeds the next frame's extraction";
        services.BeginWorldDrawFrame();
        EXPECT_FALSE(participates()) << "the view sleeps again without a new request";
    }
    services.Shutdown();
}

// Registers a default PBR material with a synchronous compile, so its pipeline exists before the first frame.
GUID RegisterCubeMaterial(RenderServices& services, const std::filesystem::path& shaderDir)
{
    MaterialBuildContext build{};
    build.AdapterShaderDir = shaderDir;
    build.CacheRoot = std::filesystem::temp_directory_path() / "ge_staging_view_cache";
    build.IncludeDirs = {shaderDir};
    services.Materials().SetMaterialBuildContext(build);
    const GUID materialGuid = GUID::Generate();
    const Engine::Renderer::Material* material = services.Materials().RegisterMaterialFromDocument(
        materialGuid, MaterialDocument::CreateDefaultPBR("Staging.Cube"), MaterialKeyword::None,
        Engine::Renderer::MaterialSystem::BaseCompileMode::Synchronous);
    if (!material || !material->GetGraphicsPipelineId().IsValid())
        return GUID{};
    return materialGuid;
}

// The engine's camera two units in front of the origin, looking at the cube.
Engine::Renderer::Camera MakeCubeCamera()
{
    constexpr float kFovDegrees = 45.0f;
    const Mathematics::Vector3 eye(0.0f, 0.0f, -2.0f);
    const Mathematics::Vector3 target(0.0f, 0.0f, 0.0f);
    const Mathematics::Vector3 up(0.0f, 1.0f, 0.0f);
    Engine::Renderer::Camera camera{};
    const Mathematics::Matrix4x4 pose = Mathematics::Inverse(Mathematics::MakeLookAtLH(eye, target, up));
    camera.SetPose(pose, Mathematics::WorldPosition{pose[3][0], pose[3][1], pose[3][2]});
    camera.params.FovY = kFovDegrees;
    return camera;
}

void SpawnCube(RenderServices& services, ECS::World& world, const GUID& materialGuid)
{
    const ECS::EntityHandle cube = world.CreateEntity();
    Components::MeshRenderer renderer = Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(
        &services, Engine::Renderer::PrimitiveGenerator::CubeGuid(), materialGuid);
    renderer.RenderLayers = RenderLayerMask::Default();
    renderer.shadowCasting = Components::ShadowCasting::Off;
    world.AddComponentImmediate(cube, renderer);
    world.AddComponentImmediate(cube, Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(
                                          &services, Engine::Renderer::PrimitiveGenerator::CubeGuid()));
    world.AddComponentImmediate(cube, Components::Transform{});
    world.ProcessCommands();
}

// Renders staging-view frames on the shared device: one request, one world pass and one output readback per frame.
class StagingFrameRunner
{
  public:
    static constexpr uint32_t kCenter = kSize / 2;

    StagingFrameRunner(IDevice* device, RenderServices& services)
        : m_Device(device), m_Services(services), m_Extraction(&services), m_Persistent(device),
          m_Transient(device), m_Ring(device, 2, 262144), m_Frame(device, &m_Persistent, &m_Transient, &m_Ring),
          m_Registration(services.Spine(), m_Frame)
    {
    }

    // Without extraction the frame draws the view's current draw lists, so a stale list shows.
    bool Render(StagingView& view, bool extract, ViewReadbackResult& image)
    {
        if (!m_Device->BeginFrame())
            return false;
        m_Frame.BeginFrame(m_FrameIndex++);
        m_Services.Views().RequestViewFrame(view.ViewId());
        if (extract)
        {
            ECS::World& world = view.World();
            m_Services.BeginWorldDrawFrame();
            m_Hierarchy.Update(world, 0.0f);
            m_Extraction.Update(world, 0.0f);
            m_Services.BuildWorldBatchKeys();
        }
        RenderServices::FrameGraphBuildParamsRG params{};
        m_Services.Spine().BuildFrameGraph(m_Frame, params);

        const auto worldPass = view.DeclareWorldPass(m_Frame, MaterialKeyword::Instanced);
        if (!worldPass.IsValid())
            return false;
        Passes::TonemapParams tonemap{};
        tonemap.preserveAlpha = 1;
        if (!view.DeclareOutput(m_Frame, worldPass.Color, tonemap))
            return false;
        // The import dedups by texture handle, so it reads the output that DeclareOutput wrote.
        const RenderGraph::RGTexture output =
            m_Frame.ImportExternalTexture(view.UiTextureName().c_str(), view.OutputTexture(),
                                          ResourceState::ShaderResource, TextureFormat::R16G16B16A16_FLOAT);
        const auto ticket = RequestTextureReadbackRG(m_Device, m_Frame, output, "Staging.Readback");
        if (!ticket)
            return false;
        m_Frame.Execute();
        OnFrameSubmittedReadbacksRG(m_Frame, m_Frame.SubmissionToken());
        m_Device->Present();
        m_Device->WaitForIdle();
        return ticket->TryGet(image);
    }

    // The default material can take a few frames to reach a drawable pipeline.
    bool RenderUntilCubeCoversCenter(StagingView& view, ViewReadbackResult& image)
    {
        constexpr uint32_t kMaxFrames = 16;
        for (uint32_t frame = 0; frame < kMaxFrames; ++frame)
        {
            if (!Render(view, true, image))
                return false;
            if (HalfAt(image, kCenter, kCenter, 3) > 0.99f)
                return true;
        }
        return false;
    }

  private:
    IDevice* m_Device;
    RenderServices& m_Services;
    TransformHierarchySystem m_Hierarchy;
    RenderExtractionSystem m_Extraction;
    RenderGraph::RGResourcePool m_Persistent;
    RenderGraph::RGTransientPool m_Transient;
    RenderGraph::RGUploadRing m_Ring;
    RenderGraph::RGFrame m_Frame;
    GameEngine::Testing::ScopedPipelineFrame m_Registration;
    uint32_t m_FrameIndex = 0;
};

// A cube spawned into the staging world renders over the cleared color.
TEST(StagingViewPoolTests, ARequestedFrameRendersTheSpawnedCubeOverATransparentClear)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const std::filesystem::path shaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
    if (!std::filesystem::exists(shaderDir / "Adapters"))
        GTEST_SKIP() << "staged shader sources not found at " << shaderDir.string();
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    GameEngine::Testing::PinWorldOnlyPipeline(services.Spine());
    Engine::Renderer::PrimitiveGenerator::RegisterAll(services);
    {
        StagingViewPool pool(services);
        StagingView view = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Frame"});
        const GUID materialGuid = RegisterCubeMaterial(services, shaderDir);
        ASSERT_FALSE(materialGuid.IsNull()) << "the cube's material did not compile";
        SpawnCube(services, view.World(), materialGuid);
        view.SetCamera(MakeCubeCamera());

        StagingFrameRunner runner(device, services);
        ViewReadbackResult image{};
        ASSERT_TRUE(runner.RenderUntilCubeCoversCenter(view, image)) << "the cube never covered the center of the output";
        ASSERT_EQ(image.width, kSize);
        ASSERT_EQ(image.height, kSize);
        ASSERT_EQ(image.format, TextureFormat::R16G16B16A16_FLOAT);
        constexpr uint32_t kCenter = StagingFrameRunner::kCenter;
        for (const uint32_t at : {0u, kSize - 1})
        {
            EXPECT_NEAR(HalfAt(image, at, at, 3), 0.0f, 1e-3f) << "the corners clear to alpha 0";
            EXPECT_NEAR(HalfAt(image, at, at, 0), 0.0f, 1e-3f) << "the corners clear to black";
        }
        EXPECT_NEAR(HalfAt(image, kCenter, kCenter, 3), 1.0f, 1e-3f) << "the opaque cube covers the center";
    }
    services.Shutdown();
}

// A released entry drops its view's draw lists: the next owner of the view renders only its own world.
TEST(StagingViewPoolTests, AReleasedAndReacquiredViewRendersNothingStale)
{
    IDevice* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const std::filesystem::path shaderDir = GameEngine::TestPaths::StagedRenderingShadersDir();
    if (!std::filesystem::exists(shaderDir / "Adapters"))
        GTEST_SKIP() << "staged shader sources not found at " << shaderDir.string();
    RenderServices services;
    ASSERT_TRUE(services.Initialize(device));
    GameEngine::Testing::PinWorldOnlyPipeline(services.Spine());
    Engine::Renderer::PrimitiveGenerator::RegisterAll(services);
    {
        StagingViewPool pool(services);
        StagingView first = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.First"});
        const GUID materialGuid = RegisterCubeMaterial(services, shaderDir);
        ASSERT_FALSE(materialGuid.IsNull()) << "the cube's material did not compile";
        SpawnCube(services, first.World(), materialGuid);
        first.SetCamera(MakeCubeCamera());

        StagingFrameRunner runner(device, services);
        ViewReadbackResult image{};
        ASSERT_TRUE(runner.RenderUntilCubeCoversCenter(first, image)) << "the cube never covered the center of the output";

        const ViewId view = first.ViewId();
        first = StagingView{};
        EXPECT_TRUE(services.GetWorldDrawBuilder().GetSubmissions(view).empty()) << "a released view keeps no draw lists";
        StagingView reused = pool.Acquire(StagingViewDesc{kSize, kSize, "Staging.Reused"});
        ASSERT_EQ(reused.ViewId(), view);
        reused.SetCamera(MakeCubeCamera());
        ASSERT_TRUE(runner.Render(reused, false, image));
        EXPECT_NEAR(HalfAt(image, StagingFrameRunner::kCenter, StagingFrameRunner::kCenter, 3), 0.0f, 1e-3f)
            << "the reused view draws the cube of the released owner";
    }
    services.Shutdown();
}
} // namespace
