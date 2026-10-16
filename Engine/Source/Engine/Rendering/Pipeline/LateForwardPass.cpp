#include "Engine/Rendering/Pipeline/LateForwardPass.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"

namespace GameEngine::Engine::Renderer::Pipeline
{
using namespace ::GameEngine::Rendering;

void LateForwardPass::Declare(ViewDeclare& d, const Spec& spec)
{
    Engine::Renderer::RenderServices::WorldPassTargetsRG targets{};
    targets.Color = spec.Color;
    targets.Resolve = {};
    targets.DepthResolved = d.ViewDepthResolved;
    targets.ResolveFromPipeline = false;
    targets.SkyBackdrop = spec.SkyBackdrop;
    // The view depth is attached when it matches the colour's sample count and extent: a
    // multisampled depth beside a 1-sample colour makes DeriveFormatKey report the whole
    // pass as multisampled and trips the SetPipeline sample-count assert. Under MSAA the
    // colour here is a resolved 1-sample target, so the pass attaches a 1-sample copy of
    // the view depth instead (DeclareDepthCopy).
    const uint32_t viewKey = static_cast<uint32_t>(d.View.id);
    if (d.ViewDepth.IsValid() && targets.Color.IsValid())
    {
        const auto colorDesc = d.Frame.Graph().ResourceDesc(targets.Color.Id);
        const auto depthDesc = d.Frame.Graph().ResourceDesc(d.ViewDepth.Id);
        const bool viewDepthFits = depthDesc.SampleCount == colorDesc.SampleCount &&
                                   depthDesc.Width == colorDesc.Width && depthDesc.Height == colorDesc.Height;
        targets.Depth = viewDepthFits ? d.ViewDepth : DeclareDepthCopy(d, colorDesc, spec.PassName);
        if (targets.Depth.IsValid())
        {
            m_DeclinedDepthByView.erase(viewKey);
        }
        else
        {
            // Declining is invisible in the frame — the pass simply loses its z-test — so say
            // so once, naming what the viewer will see.
            const DepthSpec declined{colorDesc.SampleCount, depthDesc.SampleCount,
                                     colorDesc.Width,       colorDesc.Height,
                                     depthDesc.Width,       depthDesc.Height};
            const auto it = m_DeclinedDepthByView.find(viewKey);
            if (it == m_DeclinedDepthByView.end() || !(it->second == declined))
            {
                m_DeclinedDepthByView[viewKey] = declined;
                Logger::Log::Debug(
                    "{}(view {}): late transparents have NO depth test — the "
                    "view depth ({} samples, {}x{}) cannot share a pass with the colour they "
                    "composite into ({} samples, {}x{}). Smoke and ocean spray will draw over "
                    "geometry that occludes them.",
                    spec.NodeType, viewKey, depthDesc.SampleCount, depthDesc.Width, depthDesc.Height,
                    colorDesc.SampleCount, colorDesc.Width, colorDesc.Height);
            }
        }
    }
    // Unlit particles divide by the view's exposure (view_exposure.glsl), which needs the
    // metered scale: materialize the view's exposure history now, before the metering node
    // does, so the pass binds it by name and declares its read with the view's other
    // published buffers. A graph that declares no ExposureHistory binds the zero fallback.
    d.ResolveBuffer(Names::Res::ExposureHistory);
    const auto commands = d.Services.GetLateForwardCommands(d.View.id);
    // Read-only: these contributors test depth and never own it. The producer is
    // registered writesDepth=false (RenderExtractionSystem), and a ReadWrite attach would
    // declare a depth WRITE the graph would then order every later depth reader against.
    d.Services.AddForwardCommandPassForView(d.Frame, d.View.id, targets, spec.Keywords, commands, {},
                                            spec.PassName, {}, RenderGraph::RGDepthAccess::ReadOnly);
}

RenderGraph::RGTexture LateForwardPass::DeclareDepthCopy(ViewDeclare& d, const RenderGraph::RGResourceDesc& colorDesc,
                                                         const std::string& passName)
{
    // The source is the multisampled view depth as it stands when this pass runs, so the copy
    // holds everything the opaque passes wrote — the prepass, terrain, grass, the ocean and the
    // world pass — as the single-sample path's direct attach does. Sample 0 of each pixel: the
    // sample every single-sample reader of the view depth takes. View.DepthResolved is not the
    // source: it is resolved right after the prepass. The copy needs the same extent; a view
    // whose depth differs in extent has no copy and its late transparents no depth test.
    const RenderGraph::RGTexture source = d.ViewDepth;
    const auto& sourceDesc = d.Frame.Graph().ResourceDesc(source.Id);
    if (sourceDesc.SampleCount <= 1u || sourceDesc.Width != colorDesc.Width ||
        sourceDesc.Height != colorDesc.Height || colorDesc.SampleCount != 1u)
        return {};
    if (!m_DepthCopy.EnsureLoaded(d.Services.GetDevice(), /*multisampledSource=*/true))
        return {};

    TextureDesc desc{};
    desc.width = colorDesc.Width;
    desc.height = colorDesc.Height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    desc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil);
    const std::string depthName = passName + "Depth";
    const RenderGraph::RGTexture copy = d.Frame.CreateTexture(d.PassName(depthName.c_str()).c_str(), desc);
    if (!copy.IsValid())
        return {};
    const std::string copyName = passName + "DepthCopy";
    m_DepthCopy.Declare(d.Frame, source, copy, colorDesc.Width, colorDesc.Height, d.PassName(copyName.c_str()));
    return copy;
}

} // namespace GameEngine::Engine::Renderer::Pipeline
