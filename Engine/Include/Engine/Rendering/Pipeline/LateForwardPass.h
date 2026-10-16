#pragma once

#include "Engine/Rendering/Pipeline/DepthUpsamplePass.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer::Pipeline
{
// Draws a view's late forward commands (the particles; RenderServices::GetLateForwardCommands)
// over a colour target that already holds the scene: the late transparent block of a node.
// TransmissiveRender draws them over the lit scene before the post-processes; PostFogTransparent
// draws them over the fogged scene, each fogging itself.
//
// They depth-test against the view's opaque depth, attached read-only: they test it and never
// own it. The view depth is attached when it matches the colour's sample count and extent; under
// MSAA the colour is a resolved 1-sample target, so a 1-sample copy of the view depth is attached
// instead (DeclareDepthCopy). A view where neither fits draws them with no depth test and says so
// once.
class LateForwardPass
{
  public:
    struct Spec
    {
        // The target they blend into; the pass loads and stores it.
        Rendering::RenderGraph::RGTexture Color{};
        // The sky's backdrop, for keywords that fog what they draw (HeightFog); invalid otherwise.
        Rendering::RenderGraph::RGTexture SkyBackdrop{};
        Rendering::MaterialKeyword Keywords = Rendering::MaterialKeyword::None;
        // The pass's name; its depth copy is named after it ("<name>Depth", "<name>DepthCopy").
        const char* PassName = "LateTransparent";
        // The node's type, for the one diagnostic.
        const char* NodeType = "";
    };

    void Declare(ViewDeclare& d, const Spec& spec);

  private:
    // The depth the late transparents test against when the view's depth cannot share their pass:
    // under MSAA they composite into a resolved 1-sample colour, so sample 0 of each pixel of the
    // multisampled view depth, as it stands when they run, is copied into a 1-sample D32
    // attachment. Invalid when the view depth is not a same-extent multisampled source; the pass
    // then has no depth test.
    Rendering::RenderGraph::RGTexture DeclareDepthCopy(ViewDeclare& d,
                                                       const Rendering::RenderGraph::RGResourceDesc& colorDesc,
                                                       const std::string& passName);

    // Sample counts + extents of a late-depth attach the pass DECLINED, per view. The decline is
    // silent to the frame (the pass just loses its z-test), so the diagnostic fires on entering
    // the declining state and on any re-spec while in it — a resize or MSAA toggle — rather than
    // every frame. An entry is dropped when the attach succeeds again.
    struct DepthSpec
    {
        uint32_t ColorSamples = 0;
        uint32_t DepthSamples = 0;
        uint32_t ColorWidth = 0;
        uint32_t ColorHeight = 0;
        uint32_t DepthWidth = 0;
        uint32_t DepthHeight = 0;
        bool operator==(const DepthSpec&) const = default;
    };

    std::unordered_map<uint32_t, DepthSpec> m_DeclinedDepthByView;
    DepthUpsamplePass m_DepthCopy;
};

} // namespace GameEngine::Engine::Renderer::Pipeline
