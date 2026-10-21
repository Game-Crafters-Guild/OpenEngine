// The shipped pre-exposure snapshot and the textures it scales. The PreExposureSnapshot pass, read
// from ForwardPlus.rendergraph and run as it ships, writes the view's exposure rounded to a power of
// two into the view's ExposureHistory as P, and the pipeline names that buffer as the pre-exposure of
// exactly the textures holding scene colour: the view's colour target, the graph's "preExposed"
// textures and the ones a node marks (what the editor's resource capture divides). A view with no sky
// gets its clear colour drawn at P by SkyRender's ClearColorBackdrop.
// Part of RenderPipelineDeclareTests.
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/HalfFloat.h"
#include "RGPassQuery.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "TestDeviceHelper.h"
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
constexpr const char* kPipelineName = "PreExposureDeclare";

nlohmann::json ShippedForwardPlus()
{
    const std::filesystem::path path = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
                                       "Engine/Assets/RenderPipelines/ForwardPlus.rendergraph";
    std::ifstream in(path);
    EXPECT_TRUE(in.is_open()) << path;
    std::stringstream text;
    text << in.rdbuf();
    return nlohmann::json::parse(text.str());
}

// The shipped snapshot pass with the shipped ExposureHistory, SceneColor and LDRFinal resources.
RenderPipelineBlueprint SnapshotBlueprint()
{
    const nlohmann::json pipeline = ShippedForwardPlus();
    RenderPipelineBlueprint bp;
    bp.pipelineName = kPipelineName;
    for (const nlohmann::json& pass : pipeline["passes"])
    {
        if (pass.value("id", std::string()) != "PreExposureSnapshot")
            continue;
        RenderPipelineBlueprint::Pass p;
        p.id = "PreExposureSnapshot";
        p.type = pass.value("type", std::string());
        p.enabled = true;
        p.perView = true;
        p.passJson = pass.dump();
        bp.passes.push_back(p);
    }
    EXPECT_EQ(bp.passes.size(), 1u) << "ForwardPlus.rendergraph has no PreExposureSnapshot pass";
    RenderPipelineBlueprint::Pass probe;
    probe.id = "Probe";
    probe.type = "PreExposedProbe";
    probe.enabled = true;
    probe.perView = true;
    probe.passJson = R"({"id":"Probe","type":"PreExposedProbe"})";
    bp.passes.push_back(probe);
    for (const char* name : {"ExposureHistory", "SceneColor", "LDRFinal"})
    {
        EXPECT_TRUE(pipeline["resources"].contains(name)) << name;
        bp.resources.push_back({name, pipeline["resources"][name].dump()});
    }
    return bp;
}

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

// Resolves a "preExposed" blueprint texture and one that is not, and creates two textures of its
// own, marking one: the resource ids the pre-exposure query is asked about.
struct ProbeIds
{
    uint32_t SceneColor = RenderGraph::kInvalidId;
    uint32_t Ldr = RenderGraph::kInvalidId;
    uint32_t NodeMarked = RenderGraph::kInvalidId;
    uint32_t NodeUnmarked = RenderGraph::kInvalidId;
    // What the node itself is told while it declares (ViewDeclare::IsPreExposed).
    bool SceneColorReadsPreExposed = false;
    bool LdrReadsPreExposed = true;
    bool MarkedReadsPreExposed = false;
};

class PreExposedProbeNode final : public IRenderPipelineNode
{
  public:
    explicit PreExposedProbeNode(ProbeIds& ids) : m_Ids(ids) {}
    const char* GetTypeName() const override { return "PreExposedProbe"; }
    bool Initialize(std::string, std::string, std::string*) override { return true; }
    void DeclareForView(ViewDeclare& d) override
    {
        m_Ids.SceneColor = d.ResolveTexture("SceneColor").Id;
        m_Ids.Ldr = d.ResolveTexture("LDRFinal").Id;
        const TextureDesc desc = TargetDesc(TextureFormat::R16G16B16A16_FLOAT,
                                            static_cast<uint32_t>(TextureUsage::RenderTarget));
        const RenderGraph::RGTexture marked = d.Frame.ImportPersistentTexture("PE.NodeMarked", desc);
        d.MarkPreExposed(marked);
        m_Ids.NodeMarked = marked.Id;
        m_Ids.SceneColorReadsPreExposed = d.IsPreExposed(d.ResolveTexture("SceneColor"));
        m_Ids.LdrReadsPreExposed = d.IsPreExposed(d.ResolveTexture("LDRFinal"));
        m_Ids.MarkedReadsPreExposed = d.IsPreExposed(marked);
        m_Ids.NodeUnmarked = d.Frame.ImportPersistentTexture("PE.NodeUnmarked", desc).Id;
    }

  private:
    ProbeIds& m_Ids;
};

struct SnapshotFrame
{
    bool Declared = false;
    bool ViewColorNamesTheExposure = false;
    bool SceneColorNamesTheExposure = false;
    bool LdrNamesNoExposure = false;
    bool NodeMarkedNamesTheExposure = false;
    bool NodeUnmarkedNamesNoExposure = false;
    ExposureReadbackFeature::State State{};
};

// One frame of a sky-less view: whether SkyRender declared the clear-colour backdrop, where, how the
// world pass then attaches the view's colour target, and what that target holds afterwards.
struct BackdropFrame
{
    bool Executed = false;
    uint32_t BackdropPasses = 0;
    bool BackdropBeforeWorld = false;
    bool WorldLoadsColour = false;
    float Texel[4] = {};
};

// The snapshot blueprint followed by the shipped per-view ViewParamsUpload, SkyRender and WorldRender.
RenderPipelineBlueprint SkylessBlueprint()
{
    RenderPipelineBlueprint bp = SnapshotBlueprint();
    for (const char* type : {"ViewParamsUpload", "SkyRender", "WorldRender"})
    {
        RenderPipelineBlueprint::Pass p;
        p.id = type;
        p.type = type;
        p.enabled = true;
        p.perView = true;
        p.passJson = nlohmann::json{{"id", type}, {"type", type}}.dump();
        bp.passes.push_back(p);
    }
    return bp;
}

struct PreExposureFixture
{
    std::unique_ptr<IDevice> Device;
    std::unique_ptr<RenderServices> Rs;
    RenderPipelineNodeRegistry Registry;
    Rendering::ViewId View{};
    ProbeIds Ids;

    // `staticExposure` is the view's manual exposure scale (post-process Exposure, auto exposure off).
    bool Up(float staticExposure = 1.0f)
    {
        Device = CreateVulkanDeviceFast();
        if (!Device)
            return false;
        Rs = std::make_unique<RenderServices>();
        EXPECT_TRUE(Rs->Initialize(Device.get()));
        const CameraId cam = Rs->Views().AllocateCamera("PreExposureCam");
        Rs->Views().SetCameraData(cam, CameraData{});
        View = Rs->Views().AllocateView("PreExposureView", cam);
        Rs->Views().SetViewRenderLayerMask(View, GameEngine::Rendering::RenderLayerMask::Default());
        Rs->Views().SetViewTargets(View, 0, 0, 0, Rendering::ViewClearConfig{});
        PostProcessSettings pp{};
        pp.Exposure = staticExposure;
        pp.AutoExposureActive = false;
        Rs->Views().SetViewPostProcessOverride(View, pp);
        Nodes::RegisterEngineNodeTypes(Registry);
        EXPECT_TRUE(Registry.Register(
            "PreExposedProbe", [this] { return std::make_unique<PreExposedProbeNode>(Ids); }, true));
        return true;
    }

    // One frame of the shipped snapshot, then the view's ExposureHistory read back.
    SnapshotFrame Run()
    {
        SnapshotFrame out;
        RenderPipelineInstance instance(*Rs, Registry);
        instance.SetBlueprint(SnapshotBlueprint());

        RenderGraph::RGResourcePool persistent(Device.get());
        RenderGraph::RGTransientPool transient(Device.get());
        RenderGraph::RGUploadRing ring(Device.get(), 2, 262144);
        RenderGraph::RGFrame frame(Device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(0);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
            "PE.Color", TargetDesc(TextureFormat::R16G16B16A16_FLOAT,
                                   static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
            "PE.Depth", TargetDesc(TextureFormat::D32_FLOAT,
                                   static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));

        Rs->BeginWorldDrawFrame();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{View, color, depth, color}};
        const std::vector<Rendering::ViewDesc> views(Rs->Views().GetViews().begin(), Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        const RenderGraph::RGBuffer exposure = instance.FindPreExposureSource(Ids.SceneColor);
        out.ViewColorNamesTheExposure = exposure.IsValid() && instance.FindPreExposureSource(color.Id).Id == exposure.Id;
        out.SceneColorNamesTheExposure = exposure.IsValid();
        out.LdrNamesNoExposure = !instance.FindPreExposureSource(Ids.Ldr).IsValid();
        out.NodeMarkedNamesTheExposure = exposure.IsValid() && instance.FindPreExposureSource(Ids.NodeMarked).Id == exposure.Id;
        out.NodeUnmarkedNamesNoExposure = !instance.FindPreExposureSource(Ids.NodeUnmarked).IsValid();
        if (!exposure.IsValid())
            return out;
        frame.MarkOutput(exposure);
        const BufferHandle physical = frame.PhysicalBuffer(exposure);
        frame.Execute();
        Device->WaitForIdle();
        out.Declared = true;

        const BufferHandle readback = Device->CreateReadbackBuffer(ExposureReadbackFeature::kStateBytes);
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateBufferBarrier(physical, ResourceState::UnorderedAccess,
                                                         ResourceState::CopySource));
        cl->CopyBuffer(physical, readback, ExposureReadbackFeature::kStateBytes);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        Device->ExecuteCommandLists(lists);
        Device->WaitForIdle();
        if (const void* bytes = Device->MapBuffer(readback))
        {
            std::memcpy(&out.State, bytes, sizeof(out.State));
            Device->UnmapBuffer(readback);
        }
        Device->DestroyBuffer(readback);
        return out;
    }

    // One frame of SkylessBlueprint for the view cleared to `clear`, then its colour target read back.
    BackdropFrame RunSkyless(float clear)
    {
        BackdropFrame out;
        Rs->Views().SetViewTargets(View, 0, 0, 0, Rendering::ViewClearConfig(true, clear, clear, clear, 1.0f, true));
        RenderPipelineInstance instance(*Rs, Registry);
        instance.SetBlueprint(SkylessBlueprint());

        RenderGraph::RGResourcePool persistent(Device.get());
        RenderGraph::RGTransientPool transient(Device.get());
        RenderGraph::RGUploadRing ring(Device.get(), 2, 262144);
        RenderGraph::RGFrame frame(Device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(0);
        const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
            "PE.Color", TargetDesc(TextureFormat::R16G16B16A16_FLOAT,
                                   static_cast<uint32_t>(TextureUsage::RenderTarget | TextureUsage::ShaderResource)));
        const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
            "PE.Depth", TargetDesc(TextureFormat::D32_FLOAT,
                                   static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));

        Rs->BeginWorldDrawFrame();
        Rs->BuildWorldBatchKeys();
        const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{View, color, depth, color}};
        const std::vector<Rendering::ViewDesc> views(Rs->Views().GetViews().begin(), Rs->Views().GetViews().end());
        instance.Declare(frame, targets, views);

        namespace RGQuery = GameEngine::Testing::RGQuery;
        const auto& graph = frame.Graph();
        const auto backdrops = RGQuery::DeclaredIds(graph, RGQuery::Family{"ClearColorBackdrop"});
        out.BackdropPasses = static_cast<uint32_t>(backdrops.size());
        RenderGraph::RGPassId worldPass = RenderGraph::kInvalidId;
        for (const RenderGraph::RGAttachmentRec& rec : frame.Attachments())
        {
            const bool isBackdrop = std::find(backdrops.begin(), backdrops.end(), rec.Pass) != backdrops.end();
            if (rec.IsDepth || rec.Tex != color.Id || isBackdrop || worldPass != RenderGraph::kInvalidId)
                continue;
            worldPass = rec.Pass;
            out.WorldLoadsColour = rec.Ops.Load == RenderGraph::RGLoadOp::Load;
        }
        out.BackdropBeforeWorld = backdrops.size() == 1 && worldPass != RenderGraph::kInvalidId &&
                                  backdrops[0] < worldPass;

        frame.MarkOutput(color);
        const TextureHandle physical = frame.PhysicalTexture(color);
        frame.Execute();
        Device->WaitForIdle();
        out.Executed = true;

        constexpr size_t kTexelBytes = 4 * sizeof(uint16_t);
        const BufferHandle readback = Device->CreateReadbackBuffer(kSize * kSize * kTexelBytes);
        const ResourceState state = persistent.GetState("PE.Color");
        auto cl = Device->CreateCommandList(IDevice::QueueType::Graphics);
        cl->Begin();
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(physical, state, ResourceState::CopySource));
        cl->CopyTextureToBuffer(physical, readback, kSize, kSize);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        Device->ExecuteCommandLists(lists);
        Device->WaitForIdle();
        if (const auto* halves = static_cast<const uint16_t*>(Device->MapBuffer(readback)))
        {
            // The centre texel; the backdrop is a constant fill.
            const size_t texel = (kSize / 2) * kSize + kSize / 2;
            for (size_t c = 0; c < 4; ++c)
                out.Texel[c] = GameEngine::Mathematics::HalfToFloat(halves[texel * 4 + c]);
            Device->UnmapBuffer(readback);
        }
        Device->DestroyBuffer(readback);
        return out;
    }

    void Down()
    {
        Device->WaitForIdle();
        Rs->Shutdown();
        Device->Shutdown();
    }
};
} // namespace

// Manual exposure 3: P is the nearest power of two, 4, from the first frame on, with a history ratio
// of 1 on that frame. A shipped pass still pinned to its push-constant defaults would leave P at 1.
TEST(PreExposureDeclare, TheShippedSnapshotStoresTheViewsExposureAsAPowerOfTwo)
{
    PreExposureFixture f;
    if (!f.Up(3.0f))
        GTEST_SKIP() << "No Vulkan device available";
    const SnapshotFrame r = f.Run();
    ASSERT_TRUE(r.Declared) << "the snapshot did not resolve the view's ExposureHistory";
    EXPECT_EQ(r.State.PreExposure, 4.0f) << "P is the view's exposure rounded to a power of two";
    EXPECT_EQ(r.State.PreviousPreExposure, 4.0f) << "the first frame's history ratio is 1";
    EXPECT_EQ(ExposureReadbackFeature::ResolvePreExposure(r.State), 4.0f);
    f.Down();
}

TEST(PreExposureDeclare, OnlyTexturesHoldingSceneColourNameTheViewsExposure)
{
    PreExposureFixture f;
    if (!f.Up())
        GTEST_SKIP() << "No Vulkan device available";
    const SnapshotFrame r = f.Run();
    EXPECT_TRUE(r.SceneColorNamesTheExposure) << "SceneColor is declared preExposed";
    EXPECT_TRUE(r.ViewColorNamesTheExposure) << "the view's colour target is scene colour";
    EXPECT_TRUE(r.NodeMarkedNamesTheExposure) << "a texture a node creates and marks (a TAA history)";
    EXPECT_TRUE(r.LdrNamesNoExposure) << "LDRFinal is display-referred, after the tonemap";
    EXPECT_TRUE(r.NodeUnmarkedNamesNoExposure) << "a node's texture is absolute unless it marks it";
    // A node asking while it declares gets the same answer (a fullscreen pass's history and
    // ping-pong chain follow its output).
    EXPECT_TRUE(f.Ids.SceneColorReadsPreExposed);
    EXPECT_FALSE(f.Ids.LdrReadsPreExposed);
    EXPECT_TRUE(f.Ids.MarkedReadsPreExposed);
    f.Down();
}

TEST(PreExposureDeclare, AnUnwrittenPreExposureReadsAsOne)
{
    // The fallback buffer and a view whose graph has no snapshot hold zeros: absolute scene colour.
    EXPECT_EQ(ExposureReadbackFeature::ResolvePreExposure(ExposureReadbackFeature::State{}), 1.0f);
    ExposureReadbackFeature::State written{};
    written.PreExposure = 0.25f;
    EXPECT_EQ(ExposureReadbackFeature::ResolvePreExposure(written), 0.25f);
}

// A view with no sky and a 0.02 clear at manual exposure 3 (P = 4): SkyRender declares one
// ClearColorBackdrop pass before the world pass, the world pass loads the colour target instead of
// clearing it, and the target holds the clear colour times P. A black clear declares no pass (0 x P
// is 0) and the world pass clears.
TEST(PreExposureDeclare, ASkylessViewStoresItsClearColourAtThePreExposure)
{
    PreExposureFixture f;
    if (!f.Up(3.0f))
        GTEST_SKIP() << "No Vulkan device available";
    const BackdropFrame grey = f.RunSkyless(0.02f);
    ASSERT_TRUE(grey.Executed);
    EXPECT_EQ(grey.BackdropPasses, 1u);
    EXPECT_TRUE(grey.BackdropBeforeWorld) << "the backdrop must be drawn before the world pass";
    EXPECT_TRUE(grey.WorldLoadsColour) << "the world pass must load the backdrop, not clear over it";
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(grey.Texel[c], 0.02f * 4.0f, 1e-4f) << "channel " << c << ": the clear colour times P";

    const BackdropFrame black = f.RunSkyless(0.0f);
    ASSERT_TRUE(black.Executed);
    EXPECT_EQ(black.BackdropPasses, 0u);
    EXPECT_FALSE(black.WorldLoadsColour);
    for (int c = 0; c < 3; ++c)
        EXPECT_EQ(black.Texel[c], 0.0f) << "channel " << c;
    f.Down();
}
