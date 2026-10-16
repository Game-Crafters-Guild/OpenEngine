#pragma once

#include "Engine/Rendering/Pipeline/LateForwardPass.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::MaterialKeyword;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Per-view node that draws the dedicated post-opaque TRANSMISSIVE pass: the glass meshes peeled
// out of the world opaque pass, composited over the lit scene (View.EffectiveColor). Refracts the
// post-opaque scene-colour grab (screen-space) when available, else the env cube. Placed AFTER the
// world node (and after OceanUnderwater) and BEFORE VolumetricFog so it reads/writes the lit SceneColor.
class TransmissivePassNode final : public IRenderPipelineNode
{
  public:
    // The node's keywords the late transparent contributors (particles, ocean spray) draw with: the
    // clustered local lights, the shadow receivers and the environment. Never screen-space AO, SSR,
    // DDGI or contact shadows: opaque screen-space terms do not describe smoke.
    static constexpr Rendering::MaterialKeyword kLateTransparentKeywords =
        Rendering::MaterialKeyword::ForwardPlus | Rendering::MaterialKeyword::Shadows | Rendering::MaterialKeyword::IBL;

    const char* GetTypeName() const override { return "TransmissiveRender"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    std::string m_Id;
    std::string m_Json;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
    LateForwardPass m_Late;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
