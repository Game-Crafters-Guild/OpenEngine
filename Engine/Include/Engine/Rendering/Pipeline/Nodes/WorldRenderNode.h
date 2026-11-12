#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/Shader/ShaderVariantKey.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::MaterialKeyword;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Parse the optional "keywords" string array out of a WorldRender pass's JSON
// into a MaterialKeyword mask. This is the single source of truth for the
// blueprint->keyword mapping: WorldRenderNode applies it at declaration time,
// and material prewarm reads it from the active blueprint so the variants it
// compiles match the keywords the world draw will actually request.
Rendering::MaterialKeyword ParseWorldPassKeywords(const std::string& passJson);

// The keywords a view's world pass takes from the view's state, added to the pass's `passKeywords`:
// GTAO, HeightFog (needs `heightFogParamsPublished`), DDGI and ParallaxStepsView. The world pass and a
// paused view's warm keys both use this. The reflection and shadow keywords come from their own passes.
Rendering::MaterialKeyword WorldPassViewKeywords(Engine::Renderer::RenderServices& services,
                                                 const Rendering::ViewDesc& view,
                                                 Rendering::MaterialKeyword passKeywords,
                                                 bool heightFogParamsPublished);

// The keywords that WorldPassViewKeywords sets from the view's state.
inline constexpr Rendering::MaterialKeyword kWorldPassViewStateKeywords =
    Rendering::MaterialKeyword::GTAO | Rendering::MaterialKeyword::HeightFog | Rendering::MaterialKeyword::DDGI |
    Rendering::MaterialKeyword::ParallaxStepsView;

// Per-view node that schedules the standard world/entities pass for each active view.
// Parses optional "keywords" from .rendergraph JSON to inject pass-level shader
// keywords (e.g. ForwardPlus, Instanced) into material variant selection at draw time.
class WorldRenderNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "WorldRender"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;
    void RequestPausedViewPipelines(const PausedViewPrewarm& prewarm) override;

  private:
    std::string m_Id;
    std::string m_Json;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
