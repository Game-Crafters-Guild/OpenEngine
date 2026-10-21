#include "Engine/Rendering/Pipeline/Nodes/ClearColorBackdrop.h"

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/Pipeline/ViewExposureHistory.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGFullscreen.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <array>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
namespace
{
// clear_color_backdrop.frag's ClearColorBlock.
struct ClearColorBlock
{
    float ClearColor[4];
};
} // namespace

bool ClearColorBackdrop::EnsurePipeline(Rendering::IDevice& device)
{
    if (!m_LoadAttempted)
    {
        m_LoadAttempted = true; // one attempt: a missing package leaves the attachment clear in place
        Rendering::ShaderPackage pkg{};
        std::string error;
        const bool loaded =
            Rendering::LoadShaderPkg("Shaders/clear_color_backdrop.shaderpkg", device.PreferredShaderSource(), pkg, &error);
        const auto vs = pkg.stageBytes.find("vs");
        const auto fs = pkg.stageBytes.find("fs");
        if (!loaded || vs == pkg.stageBytes.end() || fs == pkg.stageBytes.end() || vs->second.empty() ||
            fs->second.empty())
        {
            Logger::Log::Error("ClearColorBackdrop: clear_color_backdrop.shaderpkg did not load ({}); a sky-less "
                               "view's background is stored without the pre-exposure",
                               error);
            return false;
        }
        m_Layout = {};
        m_Layout.debugName = "ClearColorBackdrop.Set0";
        m_Layout.bindings = {{0, Rendering::DescriptorType::UniformBuffer, 1, Rendering::kShaderStageFragment},
                             {1, Rendering::DescriptorType::StorageBuffer, 1, Rendering::kShaderStageFragment}};
        Rendering::ApplyMetaImageShapeToLayout(pkg.meta, m_Layout);
        m_Pipeline = Rendering::RenderGraph::MakeFullscreenPipelineDesc("ClearColorBackdrop");
        m_Pipeline.vertexShader = vs->second;
        m_Pipeline.pixelShader = fs->second;
        m_Pipeline.descriptorSetLayouts.push_back(m_Layout);
    }
    if (!m_PipelineId.IsValid() && !m_Pipeline.vertexShader.empty())
        m_PipelineId = Rendering::PipelineDescTranslator::InternGraphics(device, m_Pipeline);
    return m_PipelineId.IsValid();
}

bool ClearColorBackdrop::Declare(ViewDeclare& d)
{
    const Rendering::ViewDesc* view = d.Services.Views().FindViewDesc(d.View.id);
    if (!view || !view->targets.clearColor)
        return false;
    const float* clear = view->targets.clearColorValue;
    if (clear[0] == 0.0f && clear[1] == 0.0f && clear[2] == 0.0f)
        return false;
    Rendering::IDevice* device = d.Services.GetDevice();
    if (!device || !EnsurePipeline(*device))
        return false;
    const Rendering::RenderGraph::RGTexture target = d.ResolveTexture(Names::View::Color);
    const PipelineBufferBindingRG exposure = ResolveViewExposureHistory(d);
    if (!target.IsValid() || !exposure.IsValid())
        return false;
    const auto block = d.Frame.AllocUpload<ClearColorBlock>();
    if (!block.Valid())
        return false;
    *block.Ptr = ClearColorBlock{{clear[0], clear[1], clear[2], clear[3]}};

    std::array<Rendering::RenderGraph::RGFullscreenBufferInput, 2> buffers{};
    buffers[0].Binding = 0;
    buffers[0].Buffer = block.Buffer;
    buffers[0].Offset = block.Offset;
    buffers[0].Size = sizeof(ClearColorBlock);
    buffers[1].Binding = 1;
    buffers[1].IsStorage = true;
    buffers[1].Buffer = exposure.Buffer;
    buffers[1].Offset = exposure.Offset;
    buffers[1].Size = exposure.Size;
    buffers[1].Declare = exposure.Graph;

    const std::string name = d.PassName("ClearColorBackdrop");
    Rendering::RenderGraph::RGFullscreenDesc desc{};
    desc.Name = name.c_str();
    desc.Phase = Rendering::PassPhase::kSkyRender;
    desc.Pipeline = m_PipelineId;
    desc.Layout = m_Layout;
    desc.Buffers = buffers;
    desc.Target = target;
    desc.Ops.Load = Rendering::RenderGraph::RGLoadOp::DontCare; // every texel is written
    desc.Ops.Store = Rendering::RenderGraph::RGStoreOp::Store;
    if (!Rendering::RenderGraph::AddFullscreenPass(d.Frame, desc).IsValid())
        return false;
    // The world pass loads the colour target it would otherwise clear.
    d.Services.Views().MarkViewSkyBackdropScheduled(d.View.id);
    return true;
}
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
