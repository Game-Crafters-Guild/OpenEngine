// Which package a ComputeShader node with shaderPkgFallbacks runs: the first whose workgroup the
// device can run. The depth min/max kernel at 32 x 32 (1024 invocations) runs where the device
// allows 1024; a device that allows 256 (a phone's WebGPU, held here by
// GE_WEBGPU_MAX_COMPUTE_INVOCATIONS on the native WebGPU device) runs the 16 x 16 package and not
// the 8 x 8 one. Part of RenderPipelineDeclareTests.
#include "EngineLogCapture.h"
#include "Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "SharedHeadlessDevice.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Engine::Renderer::Pipeline;

namespace
{
struct FramePools
{
    RenderGraph::RGResourcePool Persistent;
    RenderGraph::RGTransientPool Transient;
    RenderGraph::RGUploadRing Ring;
    explicit FramePools(IDevice* d) : Persistent(d), Transient(d), Ring(d, 2, 262144) {}
};

TextureDesc TargetDesc(TextureFormat format, uint32_t usage)
{
    TextureDesc d{};
    d.width = 64;
    d.height = 64;
    d.depth = 1;
    d.mipLevels = 1;
    d.arrayLayers = 1;
    d.sampleCount = 1;
    d.format = static_cast<uint32_t>(format);
    d.usage = usage;
    return d;
}

// Sets an environment variable for one scope; the WebGPU device reads it at creation.
class ScopedEnvironmentVariable
{
  public:
    ScopedEnvironmentVariable(const char* name, const char* value) : m_Name(name)
    {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
    ~ScopedEnvironmentVariable()
    {
#ifdef _WIN32
        _putenv_s(m_Name, "");
#else
        unsetenv(m_Name);
#endif
    }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

  private:
    const char* m_Name;
};

std::unique_ptr<IDevice> CreateWebGpuDevice()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::WebGPU;
    desc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc))
        return nullptr;
    return device;
}

// The shipped graphs' DepthMinMax node, as Engine/Assets/RenderPipelines/ForwardPlus.rendergraph
// declares it.
constexpr const char* kDepthMinMaxJson =
    R"json({"id":"DepthMinMax","type":"ComputeShader","shaderPkg":"Shaders/cluster_depth_minmax.shaderpkg",)json"
    R"json("shaderPkgFallbacks":["Shaders/cluster_depth_minmax_t16.shaderpkg","Shaders/cluster_depth_minmax_t8.shaderpkg"],)json"
    R"json("inputs":{"DepthTex":"View.DepthResolved"},"inputUsages":{"DepthTex":"Sampled"},)json"
    R"json("targetBuffer":"DepthMinMaxBuffer"})json";

constexpr const char* kCaptureLive = "workgroup selection capture live";

// A DepthMinMax node whose first fallback is a package that does not exist: on a device that
// cannot run the 32 x 32 package, the selection fails to load the fallback and runs again on the
// node's next declare.
constexpr const char* kMissingFallbackJson =
    R"json({"id":"DepthMinMax","type":"ComputeShader","shaderPkg":"Shaders/cluster_depth_minmax.shaderpkg",)json"
    R"json("shaderPkgFallbacks":["Shaders/does_not_exist.shaderpkg","Shaders/cluster_depth_minmax_t8.shaderpkg"],)json"
    R"json("inputs":{"DepthTex":"View.DepthResolved"},"inputUsages":{"DepthTex":"Sampled"},)json"
    R"json("targetBuffer":"DepthMinMaxBuffer"})json";

// Declares the ComputeShader node `passJson` describes for one view on `device`, `frames` times
// over one pipeline instance, and returns the engine's log lines, the first of them kCaptureLive:
// a capture that saw nothing would pass a count of zero.
std::vector<std::string> DeclareComputeNode(IDevice& device, const char* passJson, uint64_t frames)
{
    std::vector<std::string> lines;
    TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Info);
    Logger::Log::Info(kCaptureLive);
    {
        RenderServices rs;
        EXPECT_TRUE(rs.Initialize(&device));
        const ViewId viewId =
            rs.Views().AllocateView("DepthMinMaxView", rs.Views().AllocateCamera("DepthMinMaxCam"));
        rs.Views().SetViewRenderLayerMask(viewId, GameEngine::Rendering::RenderLayerMask::Default());

        RenderPipelineNodeRegistry registry;
        EXPECT_TRUE(registry.Register(
            "ComputeShader", [] { return std::make_unique<Nodes::ComputeShaderNode>(); }, true));
        RenderPipelineBlueprint bp;
        bp.pipelineName = "DepthMinMaxSelection";
        RenderPipelineBlueprint::Pass pass;
        pass.id = "DepthMinMax";
        pass.type = "ComputeShader";
        pass.enabled = true;
        pass.perView = true;
        pass.passJson = passJson;
        bp.passes.push_back(pass);

        RenderPipelineInstance instance(rs, registry);
        instance.SetBlueprint(bp);
        FramePools pools(&device);
        for (uint64_t frameIndex = 0; frameIndex < frames; ++frameIndex)
        {
            RenderGraph::RGFrame frame(&device, &pools.Persistent, &pools.Transient, &pools.Ring);
            frame.BeginFrame(frameIndex);
            const RenderGraph::RGTexture color = frame.ImportPersistentTexture(
                "Selection.Color",
                TargetDesc(TextureFormat::RGBA8_UNORM, static_cast<uint32_t>(TextureUsage::RenderTarget)));
            const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(
                "Selection.Depth",
                TargetDesc(TextureFormat::D32_FLOAT,
                           static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource)));
            rs.BeginWorldDrawFrame();
            const std::vector<ViewTargetsRG> targets = {ViewTargetsRG{viewId, color, depth, {}}};
            const std::vector<ViewDesc> views(rs.Views().GetViews().begin(), rs.Views().GetViews().end());
            instance.Declare(frame, targets, views);
        }
        device.WaitForIdle();
        rs.Shutdown();
    }
    Logger::Log::Flush();
    return lines;
}

size_t CountContaining(const std::vector<std::string>& lines, const std::string& fragment)
{
    return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
        return line.find(fragment) != std::string::npos;
    }));
}
} // namespace

TEST(ComputeShaderWorkgroupSelection, ADeviceThatAllowsTheFullWorkgroupRunsThe32Package)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_GE(device->GetCapabilities().maxComputeWorkGroupSize, 1024u);
    const std::vector<std::string> lines = DeclareComputeNode(*device, kDepthMinMaxJson, 1);
    ASSERT_EQ(CountContaining(lines, kCaptureLive), 1u);
    EXPECT_EQ(CountContaining(lines, "RenderPipeline ComputeShader 'DepthMinMax'"), 0u)
        << "the node tried a fallback, or failed to load a package, on a device that runs 32 x 32";
}

TEST(ComputeShaderWorkgroupSelection, ADeviceThatAllows256InvocationsRunsThe16Package)
{
    const ScopedEnvironmentVariable clamp("GE_WEBGPU_MAX_COMPUTE_INVOCATIONS", "256");
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->GetCapabilities().maxComputeWorkGroupSize, 256u);
    const std::vector<std::string> lines = DeclareComputeNode(*device, kDepthMinMaxJson, 1);
    ASSERT_EQ(CountContaining(lines, kCaptureLive), 1u);
    EXPECT_EQ(CountContaining(lines, "'Shaders/cluster_depth_minmax.shaderpkg' needs more invocations per "
                                     "workgroup than this device's 256; trying "
                                     "'Shaders/cluster_depth_minmax_t16.shaderpkg'"),
              1u);
    EXPECT_EQ(CountContaining(lines, "trying 'Shaders/cluster_depth_minmax_t8.shaderpkg'"), 0u);
    device->Shutdown();
}

// A fallback that fails to load leaves the node without a package, so the selection runs again on
// its next declare; the fallback line is logged on the node's first selection only.
TEST(ComputeShaderWorkgroupSelection, AFallbackThatFailsToLoadIsReportedOnce)
{
    const ScopedEnvironmentVariable clamp("GE_WEBGPU_MAX_COMPUTE_INVOCATIONS", "256");
    auto device = CreateWebGpuDevice();
    ASSERT_NE(device, nullptr);
    const std::vector<std::string> lines = DeclareComputeNode(*device, kMissingFallbackJson, 2);
    ASSERT_EQ(CountContaining(lines, kCaptureLive), 1u);
    EXPECT_EQ(CountContaining(lines, "trying 'Shaders/does_not_exist.shaderpkg'"), 1u);
    device->Shutdown();
}
