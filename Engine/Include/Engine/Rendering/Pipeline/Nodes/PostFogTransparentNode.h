#pragma once

#include "Engine/Rendering/Pipeline/LateForwardPass.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Per-view node that draws the late forward commands (the particles) AFTER the HeightFog
// post-process, over the fogged scene, each fragment fogging itself by its own distance from the
// camera (MaterialKeyword::HeightFog, Includes/forward_height_fog.glsl).
//
// The post-process cannot fog them: it reads only the opaque depth, so a sprite drawn before it
// takes the fog of whatever lies behind it, too much in front of a hill, none at all over the sky.
// A pipeline with this node enabled draws them here instead of in TransmissiveRender.
//
// Placed after the fog glow (which treats whatever the fog pass's output holds beyond the dimmed
// scene as the fog's own light) and before the lens effects and TAA. JSON:
//   { "id": "PostFogTransparent", "type": "PostFogTransparent", "output": "HDRFogGlow",
//     "keywords": ["ForwardPlus", "Instanced", "Shadows", "IBL"] }
// "output" is the colour they blend into (default View.Resolve); "keywords" as the world pass's,
// of which they take TransmissivePassNode::kLateTransparentKeywords.
class PostFogTransparentNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "PostFogTransparent"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_OutputRef;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
    bool m_LoggedUnresolvedOutput = false;
    LateForwardPass m_Late;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
