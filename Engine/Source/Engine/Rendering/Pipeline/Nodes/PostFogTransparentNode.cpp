#include "Engine/Rendering/Pipeline/Nodes/PostFogTransparentNode.h"

#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h" // kLateTransparentKeywords
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"      // ParseWorldPassKeywords
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool PostFogTransparentNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_OutputRef = Names::View::Resolve;
    try
    {
        const auto j = nlohmann::json::parse(nodeJson);
        if (j.is_object() && j.contains("output") && j["output"].is_string())
            m_OutputRef = j["output"].get<std::string>();
    }
    catch (const std::exception& ex)
    {
        if (outError)
            *outError = std::string("PostFogTransparent node JSON parse failed: ") + ex.what();
        return false;
    }
    m_PassKeywords = ParseWorldPassKeywords(nodeJson);
    return true;
}

void PostFogTransparentNode::DeclareForView(ViewDeclare& d)
{
    if (!d.Services.HasLateForwardCommands(d.View.id))
        return;

    RenderGraph::RGTexture target{};
    if (m_OutputRef == Names::View::Resolve)
        target = d.ViewResolve.IsValid() ? d.ViewResolve : d.ResolveTexture(Names::View::Resolve);
    else
        target = d.ResolveTexture(m_OutputRef);
    if (!target.IsValid())
    {
        // TransmissiveRender has left the particles to this node, so they would not draw at all.
        if (!m_LoggedUnresolvedOutput)
        {
            m_LoggedUnresolvedOutput = true;
            Logger::Log::Warning("RenderPipeline PostFogTransparent '{}': output '{}' did not resolve; the "
                                 "view's particles are not drawn",
                                 m_Id, m_OutputRef);
        }
        return;
    }

    // The fog's parameters, bound by name (the HeightFogParamsUpload node publishes them; a graph
    // without one binds the no-fog fallback), and the sky's backdrop for a fog coloured by the sky.
    d.ResolveBuffer(Names::Res::HeightFogParams);
    LateForwardPass::Spec late;
    late.Color = target;
    late.SkyBackdrop = d.ResolveTexture(Names::Res::SkyBackdrop);
    late.Keywords = (m_PassKeywords & TransmissivePassNode::kLateTransparentKeywords) | MaterialKeyword::HeightFog;
    late.PassName = "PostFogTransparent";
    late.NodeType = GetTypeName();
    m_Late.Declare(d, late);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
