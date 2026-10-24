// The view output reaches the Tonemap node: a view whose post-process settings ask for no look
// gets the shader's no-look arm, written by the node after the reflected look, not the blended look.
// Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Components/Rendering/Look.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "SharedHeadlessDevice.h"
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{

constexpr uint32_t kSize = 4;

// A scene value Neutral compresses (its curve starts at 0.76) and no look passes through.
constexpr float kSceneValue = 0.9f;

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc d{};
    d.width = kSize;
    d.height = kSize;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = 1;
    d.format = static_cast<uint32_t>(format);
    d.usage = usage;
    return d;
}

// The shipped Tonemap pass, read from ForwardPlus.rendergraph, with its input and output pointed at
// the view's own targets; everything else (push-constant defaults, ppOverrides) as it ships.
RenderPipelineBlueprint TonemapBlueprint()
{
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Assets/RenderPipelines/ForwardPlus.rendergraph";
    std::ifstream in(path);
    EXPECT_TRUE(in.is_open()) << path;
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json pipeline = nlohmann::json::parse(text.str());

    nlohmann::json tonemap;
    for (const nlohmann::json& pass : pipeline["passes"])
        if (pass.value("id", std::string()) == "Tonemap")
            tonemap = pass;
    EXPECT_FALSE(tonemap.is_null()) << "ForwardPlus.rendergraph has no Tonemap pass";
    tonemap["inputs"] = {{"uHDRColor", "View.Color"}};
    tonemap.erase("buffers");
    tonemap.erase("bufferUsages");
    tonemap["output"] = "View.Resolve";

    RenderPipelineBlueprint bp;
    bp.pipelineName = "TonemapViewOutput";
    RenderPipelineBlueprint::Pass p;
    p.id = "Tonemap";
    p.type = tonemap.value("type", std::string());
    p.enabled = true;
    p.perView = true;
    p.passJson = tonemap.dump();
    bp.passes.push_back(p);
    return bp;
}

struct TonemapFixture
{
    IDevice* Device = nullptr;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    Rendering::ViewId View{};
    // The last frame's Tonemap pass read the view's exposure compensation state (the night tint's gate).
    bool TonemapReadsCompensation = false;

    bool Up()
    {
        Device = SharedHeadlessDevice();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        EXPECT_TRUE(Rs->Initialize(Device));
        CameraData camera{};
        for (int i = 0; i < 16; i += 5)
        {
            camera.view[i] = 1.0f;
            camera.proj[i] = 1.0f;
            camera.viewProj[i] = 1.0f;
        }
        const CameraId cam = Rs->Views().AllocateCamera("TonemapViewOutputCam");
        Rs->Views().SetCameraData(cam, camera);
        View = Rs->Views().AllocateView("TonemapViewOutputView", cam);
        Rs->Views().SetViewRenderLayerMask(View, GameEngine::Rendering::RenderLayerMask::Default());
        Rs->Views().SetViewTargets(View, 0, 0, 0, Rendering::ViewClearConfig{});
        Nodes::RegisterEngineNodeTypes(Registry);
        return true;
    }

    // One frame: the view's colour filled with the scene value, the Tonemap pass from it into a
    // float target, read back. Returns the red channel of the first pixel.
    float Tonemap(PostProcessViewOutput output, bool autoExposure = false)
    {
        PostProcessSettings pp{};
        pp.Exposure = 1.0f;
        pp.AutoExposureActive = autoExposure;
        pp.Look = static_cast<int32>(Components::Look::Neutral);
        pp.ViewOutput = output;
        Rs->Views().SetViewPostProcessOverride(View, pp);

        RenderPipelineInstance instance(*Rs, Registry);
        instance.SetBlueprint(TonemapBlueprint());

        RenderGraph::RGResourcePool persistent(Device);
        RenderGraph::RGTransientPool transient(Device);
        RenderGraph::RGUploadRing ring(Device, 2, 262144);
        RenderGraph::RGFrame frame(Device, &persistent, &transient, &ring);
        frame.BeginFrame(0);

        const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
            "TVO.Color", TargetDesc(TextureFormat::R16G16B16A16_FLOAT,
                                    static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
            "TVO.Depth", TargetDesc(TextureFormat::D32_FLOAT,
                                    static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
        const TextureHandle resolveHandle = Device->CreateTexture(TargetDesc(
            TextureFormat::R32G32B32A32_FLOAT,
            static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::TransferSrc)));
        EXPECT_TRUE(resolveHandle.IsValid());
        const RenderGraph::RGTexture resolve = frame.ImportExternalTexture(
            "TVO.Resolve", resolveHandle, ResourceState::Undefined, TextureFormat::R32G32B32A32_FLOAT);

        frame.AddPass(
            "TVO.Fill", 0,
            [&](RenderGraph::RGPassBuilder& p)
            {
                RenderGraph::RGAttachmentOps ops{};
                ops.Load = RenderGraph::RGLoadOp::Clear;
                ops.Store = RenderGraph::RGStoreOp::Store;
                ops.Clear.Color[0] = ops.Clear.Color[1] = ops.Clear.Color[2] = kSceneValue;
                ops.Clear.Color[3] = 1.0f;
                p.AttachColor(0, color, ops);
            },
            [](RenderGraph::RGContext&) {});

        Rs->BeginWorldDrawFrame();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{View, color, depth, resolve}};
        const std::vector<Rendering::ViewDesc> views(Rs->Views().GetViews().begin(), Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);
        TonemapReadsCompensation = false;
        const RenderGraph::RGGraph& g = frame.Graph();
        for (const RenderGraph::RGAccessRecord& a : g.Accesses())
        {
            const char* pn = g.PassName(a.Pass);
            const char* rn = g.ResourceName(a.Resource);
            if (pn && rn && std::string(pn).find("Tonemap") != std::string::npos &&
                std::string(rn).rfind("Pipeline.AutoExposure.Compensation.View", 0) == 0 &&
                !RenderGraph::IsWrite(a.Access))
                TonemapReadsCompensation = true;
        }
        frame.MarkOutput(resolve, RenderGraph::RGImageLayout::ShaderReadOnly);
        frame.Execute();
        Device->WaitForIdle();

        const BufferHandle readback = Device->CreateReadbackBuffer(kSize * kSize * 4u * sizeof(float));
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(resolveHandle, ResourceState::ShaderResource,
                                                          ResourceState::CopySource));
        cl->CopyTextureToBuffer(resolveHandle, readback, kSize, kSize);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        Device->ExecuteCommandLists(lists);
        Device->WaitForIdle();

        float red = -1.0f;
        if (const auto* pixels = static_cast<const float*>(Device->MapBuffer(readback)))
        {
            red = pixels[0];
            Device->UnmapBuffer(readback);
        }
        Device->DestroyBuffer(readback);
        Device->DestroyTexture(resolveHandle);
        return red;
    }

    void Down()
    {
        Device->WaitForIdle();
        Rs->Shutdown();
    }
};

} // namespace

TEST(TonemapViewOutput, NoLookViewOutputSkipsTheBlendedLook)
{
    TonemapFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";

    const float look = f.Tonemap(PostProcessViewOutput::Look);
    const float noLook = f.Tonemap(PostProcessViewOutput::NoLook);

    // Neutral compresses 0.9 to about 0.83; the instrument must see that, or a pass that never ran
    // (or a black placeholder) could pass the second assertion.
    EXPECT_GT(look, 0.78f) << "the Tonemap pass did not run";
    EXPECT_LT(look, 0.87f) << "the blended look (Neutral) did not compress " << kSceneValue;
    EXPECT_NEAR(noLook, kSceneValue, 2e-3f)
        << "a view asking for no look still got the blended look (" << look << ")";

    f.Down();
}

// The night tint gates on what the view meters: under Auto exposure the shipped Tonemap pass reads the
// view's exposure compensation state with no entry in its pipeline JSON (the engine binds
// uCompensation), so every pipeline, a game's own included, reads it. Without Auto exposure nothing
// writes the state and the pass does not wait on it.
TEST(TonemapViewOutput, TheTonemapReadsTheViewsMeteredReading)
{
    TonemapFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    f.Tonemap(PostProcessViewOutput::Look, /*autoExposure=*/true);
    EXPECT_TRUE(f.TonemapReadsCompensation)
        << "the Tonemap pass must read Pipeline.AutoExposure.Compensation.View<id> (uCompensation)";
    f.Tonemap(PostProcessViewOutput::Look, /*autoExposure=*/false);
    EXPECT_FALSE(f.TonemapReadsCompensation) << "a view without Auto exposure has no compensation state to read";
    f.Down();
}
