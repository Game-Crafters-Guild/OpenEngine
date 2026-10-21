#pragma once

#include "Engine/Rendering/Pipeline/DepthUpsamplePass.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"

#include <cstdint>
#include <unordered_map>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::MaterialKeyword;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Per-view node that draws what composites over the lit scene after the opaque passes:
// - the glass meshes peeled out of the world opaque pass, composited over the lit scene
//   (View.EffectiveColor). Refracts the post-opaque scene-colour grab (screen-space) when available,
//   else the env cube. Placed after the world node and OceanUnderwater, before the post chain, so
//   it reads and writes the lit SceneColor;
// - the late transparents (particles, ocean spray), into "output" (default View.Resolve). Placed
//   after the VolumetricFog composite they take the volumetric fog by their own distance, and the
//   composite, which fogs by the opaque depth, does not fog them again.
// "draws" lists which of the two a node draws ("glass", "lateTransparents"); absent, both.
// "clouds" names the graph's resolved volumetric clouds when the late transparents draw after the
// clouds composite: a blended surface beyond the clouds then takes their transmittance.
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
    // The depth the late transparents test against when the view's depth cannot share their pass:
    // under MSAA they composite into the resolved 1-sample colour, so sample 0 of each pixel of
    // the multisampled view depth, as it stands when they run, is copied into a 1-sample D32
    // attachment. Invalid when the view depth is not a same-extent multisampled source; the pass
    // then has no depth test.
    Rendering::RenderGraph::RGTexture DeclareLateDepthCopy(ViewDeclare& d,
                                                           const Rendering::RenderGraph::RGResourceDesc& colorDesc);

    // Sample counts + extents of a late-depth attach the node DECLINED, per view. The decline is
    // silent to the frame (the pass just loses its z-test), so the diagnostic fires on entering
    // the declining state and on any re-spec while in it — a resize or MSAA toggle — rather than
    // every frame. An entry is dropped when the attach succeeds again.
    struct LateDepthSpec
    {
        uint32_t ColorSamples = 0;
        uint32_t DepthSamples = 0;
        uint32_t ColorWidth = 0;
        uint32_t ColorHeight = 0;
        uint32_t DepthWidth = 0;
        uint32_t DepthHeight = 0;
        bool operator==(const LateDepthSpec&) const = default;
    };

    std::string m_Id;
    std::string m_Json;
    Rendering::MaterialKeyword m_PassKeywords = Rendering::MaterialKeyword::None;
    bool m_DrawsGlass = true;
    bool m_DrawsLateTransparents = true;
    // The colour the late transparents composite into; empty is the view's resolve target.
    std::string m_LateOutputRef;
    // The resolved volumetric clouds the late transparents test their distance against; empty: none.
    std::string m_CloudsRef;
    std::unordered_map<uint32_t, LateDepthSpec> m_DeclinedLateDepthByView;
    DepthUpsamplePass m_LateDepthCopy;
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
