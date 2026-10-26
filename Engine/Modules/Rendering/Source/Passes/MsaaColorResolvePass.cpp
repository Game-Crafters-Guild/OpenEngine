#include "Rendering/Passes/MsaaColorResolvePass.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Logger/Logger.h"
#include "Types/FlatMap.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace GameEngine::Rendering::Passes
{
namespace
{
constexpr const char* kPackagePath = "Shaders/msaa_color_resolve.shaderpkg";

// One device's resolve program. The pipeline id names an interned description and survives a
// device rebuild; the sampler is a GPU object, so the per-device cleanup drops the entry.
struct ResolveDeviceResources
{
    GraphicsPipelineId Pipeline{};
    DescriptorSetLayoutDesc Layout{};
    SamplerHandle Sampler{};
};

std::mutex g_ResolveMutex;
FlatMap<IDevice*, ResolveDeviceResources> g_ResolvePerDevice;

void DropResolveResources(IDevice* device)
{
    std::lock_guard lock(g_ResolveMutex);
    ResolveDeviceResources* resources = g_ResolvePerDevice.Find(device);
    if (!resources)
        return;
    if (resources->Sampler.IsValid())
        device->DestroySampler(resources->Sampler);
    g_ResolvePerDevice.Erase(device);
}

bool LoadResolveResources(IDevice& device, ResolveDeviceResources& out)
{
    ShaderPackage package{};
    std::string error;
    if (!LoadShaderPkg(kPackagePath, device.PreferredShaderSource(), package, &error))
    {
        static std::atomic<bool> s_Reported{false};
        if (!s_Reported.exchange(true))
            Logger::Log::Error("MsaaColorResolvePass: {} did not load ({}); multisampled scene color is not "
                               "resolved. Build the CompileShaderPkgs target to stage it.",
                               kPackagePath, error);
        return false;
    }
    const auto vertex = package.stageBytes.find("vs");
    const auto fragment = package.stageBytes.find("fs");
    if (vertex == package.stageBytes.end() || fragment == package.stageBytes.end())
    {
        Logger::Log::Error("MsaaColorResolvePass: {} has no vs/fs stage", kPackagePath);
        return false;
    }

    out.Layout.debugName = "MsaaColorResolve.Set0";
    out.Layout.bindings = {{0, DescriptorType::CombinedImageSampler, 1, kShaderStageFragment}};
    ApplyMetaImageShapeToLayout(package.meta, out.Layout);
    PipelineDesc pipeline = RenderGraph::MakeFullscreenPipelineDesc("MsaaColorResolve");
    pipeline.vertexShader = vertex->second;
    pipeline.pixelShader = fragment->second;
    pipeline.descriptorSetLayouts.push_back(out.Layout);
    out.Pipeline = PipelineDescTranslator::InternGraphics(device, pipeline);
    // texelFetch ignores the filter; the binding is a combined image sampler and needs one.
    out.Sampler = device.CreateSampler(SamplerDesc::PointClamp("MsaaColorResolve.PointClamp"));
    return out.Pipeline.IsValid() && out.Sampler.IsValid();
}

// Copies this device's resources into `out`, loading them on first use. A failed load is
// retried on the next declaration, so staging the package mid-session recovers.
bool EnsureResolveResources(IDevice& device, ResolveDeviceResources& out)
{
    std::lock_guard lock(g_ResolveMutex);
    if (const ResolveDeviceResources* cached = g_ResolvePerDevice.Find(&device))
    {
        out = *cached;
        return true;
    }
    ResolveDeviceResources resources{};
    if (!LoadResolveResources(device, resources))
    {
        if (resources.Sampler.IsValid())
            device.DestroySampler(resources.Sampler);
        return false;
    }
    device.RegisterPerDeviceCacheCleanup("Passes.MsaaColorResolve", DropResolveResources);
    out = g_ResolvePerDevice.InsertOrAssign(&device, std::move(resources));
    return true;
}

} // namespace

RenderGraph::RGPass AddMsaaColorResolvePassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture multisampled,
                                              RenderGraph::RGTexture resolved, const char* passName)
{
    IDevice* device = frame.Device();
    if (!device || !multisampled.IsValid() || !resolved.IsValid())
        return {};
    const RenderGraph::RGResourceDesc& source = frame.Graph().ResourceDesc(multisampled.Id);
    const RenderGraph::RGResourceDesc& target = frame.Graph().ResourceDesc(resolved.Id);
    if (source.SampleCount < 2 || target.SampleCount != 1 || target.MipLevels != 1 || target.ArrayLayers != 1 ||
        source.Width != target.Width || source.Height != target.Height)
    {
        Logger::Log::Error("MsaaColorResolvePass '{}': needs a multisampled source and a single-sample, one-mip, "
                           "one-layer target of its size (source {}x{} x{}, target {}x{} x{}, {} mips, {} layers); "
                           "the pass is not declared",
                           passName, source.Width, source.Height, source.SampleCount, target.Width, target.Height,
                           target.SampleCount, target.MipLevels, target.ArrayLayers);
        return {};
    }
    ResolveDeviceResources resources{};
    if (!EnsureResolveResources(*device, resources))
        return {};

    RenderGraph::RGFullscreenTextureInput input{};
    input.Binding = 0;
    input.Texture = multisampled;
    input.Sampler = resources.Sampler;
    input.Required = true;

    RenderGraph::RGFullscreenDesc desc{};
    desc.Name = passName;
    desc.Phase = PassPhase::kWorldRender;
    desc.Pipeline = resources.Pipeline;
    desc.Layout = resources.Layout;
    desc.Textures = {&input, 1};
    desc.Target = resolved;
    desc.Ops.Load = RenderGraph::RGLoadOp::DontCare;
    desc.Ops.Store = RenderGraph::RGStoreOp::Store;
    desc.Ops.WritesEveryPixel = true;
    return RenderGraph::AddFullscreenPass(frame, desc);
}

} // namespace GameEngine::Rendering::Passes
