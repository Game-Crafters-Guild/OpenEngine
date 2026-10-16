#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"

#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h" // ParseWorldPassKeywords
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool TransmissivePassNode::Initialize(std::string nodeId, std::string nodeJson, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);
    // Same keyword set as the world pass (ForwardPlus / IBL / Shadows / Instanced) so the
    // glass variant compiles with full lighting + the IBL env-cube refraction fallback.
    m_PassKeywords = ParseWorldPassKeywords(m_Json);
    return true;
}

void TransmissivePassNode::DeclareForView(ViewDeclare& d)
{
    const bool hasGlass = d.Services.HasTransmissionInView(d.View.id);
    // A pipeline that draws the late forward commands after the height fog (PostFogTransparent)
    // leaves them to that node.
    const bool hasLateForward =
        !d.PipelineHasPostFogTransparentPass && d.Services.HasLateForwardCommands(d.View.id);
    // Glass-free and particle-free views pay nothing.
    if (!hasGlass && !hasLateForward)
        return;

    // Transparent contributors such as smoke and ocean spray render after the
    // Ocean node. Keeping them out of World prevents refraction scene-grabs from
    // treating foreground particles as background. They composite into the
    // resolved HDR colour Ocean made authoritative (rather than resolving the
    // pre-ocean MSAA colour a second time), depth-testing against the opaque depth
    // that produced it so occluded particles are hidden rather than painted over
    // the geometry in front of them.
    if (hasLateForward)
    {
        // Individual commands carry their vertex/instancing keywords. Lit
        // particles need the same clustered local lights and shadow receivers
        // as the world pass; IBL alone silently drops point and spot lights.
        LateForwardPass::Spec late;
        late.Color = d.ViewResolve;
        late.Keywords = m_PassKeywords & kLateTransparentKeywords;
        late.PassName = "LateTransparent";
        late.NodeType = GetTypeName();
        m_Late.Declare(d, late);
    }

    if (!hasGlass)
        return;

    // Composite over the LIT opaque scene the world node published (the resolved single-sample
    // colour). The shared SceneColor id orders this write after the world pass and before
    // VolumetricFog; the grab (which reads the same colour) is declared between them below.
    const auto effectiveColor = d.ResolveTexture(Names::View::EffectiveColor);
    if (!effectiveColor.IsValid())
        return;

    // Grab the lit-opaque scene colour (post-world) BEFORE the transmissive pass so the glass
    // refracts it screen-space. The grab reads View.EffectiveColor (ordering it after the world
    // pass); the transmissive pass writes the same colour (ordering it after the grab). When the
    // grab declines (e.g. a multisampled source) the SceneColorGrab keyword is dropped and the
    // glass compiles the env-cube refraction fallback instead.
    auto& grab = d.Services.GetOrCreateTransmissionSceneGrab();
    const bool grabReady = grab.DeclareForView(d, static_cast<uint32_t>(d.Frame.FrameIndex()));

    Rendering::MaterialKeyword keywords = m_PassKeywords;
    if (grabReady)
        keywords |= Rendering::MaterialKeyword::SceneColorGrab;

    // Composite into the RESOLVED single-sample colour, depth-tested against the opaque depth that
    // produced it. The attach is read-only (AddWorldPassImpl forces it for this scope), which is
    // also what suppresses depth writes: the recorder issues vkCmdSetDepthWriteEnable(VK_FALSE) on
    // a read-only pass, so the material's own depthWriteEnable cannot leak a write. Glass must not
    // write depth — it is peeled from the prepass, and a write would occlude the transmissive
    // fragments behind it in a pass that does not sort. The material pipeline already carries
    // depthTest=on with GreaterOrEqual (reverse-Z); the attachment is the only thing that turns
    // the test on.
    //
    // Attach only when the depth matches the colour's sample count and extent. A multisampled
    // depth beside the 1-sample resolved colour makes DeriveFormatKey report the whole pass as
    // multisampled and trips the SetPipeline sample-count assert. Under MSAA this pass stays
    // attachment-less and glass falls back to the in-shader compare against ge_sceneDepth
    // (adapter_forward.glsl), View.DepthResolved, which is resolved after the prepass. The late
    // transparents above make their own single-sample copy of the full view depth. DepthResolved is
    // published either way so BuildPassResourcesRG binds ge_sceneDepth for that fallback and for
    // the grab's refraction depth-texel test.
    Engine::Renderer::RenderServices::WorldPassTargetsRG t{};
    t.Color = effectiveColor;
    t.DepthResolved = d.ViewDepthResolved;
    if (d.ViewDepth.IsValid())
    {
        const auto& colorDesc = d.Frame.Graph().ResourceDesc(effectiveColor.Id);
        const auto& depthDesc = d.Frame.Graph().ResourceDesc(d.ViewDepth.Id);
        if (depthDesc.SampleCount == colorDesc.SampleCount &&
            depthDesc.Width == colorDesc.Width && depthDesc.Height == colorDesc.Height)
            t.Depth = d.ViewDepth;
    }
    // Frame-scoped grab id: the pass declares the Read(Sampled) that orders the
    // grab copy before this draw (the contract in SceneColorGrab.h).
    if (grabReady)
        t.SceneGrab = grab.GetGrabTextureRG();
    d.Services.AddWorldPassForView(
        d.Frame, d.View.id, t, keywords,
        Engine::Renderer::RenderServices::WorldPassDrawScope::TransmissiveOnly);
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
