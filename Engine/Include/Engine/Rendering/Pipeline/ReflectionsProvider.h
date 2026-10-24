// Feature-owned provider for the screen-space reflection world-pass targets.
// Everything SSR-specific about the forward MRT lives here — G-buffer slice
// formats, clear semantics, attachment locations, the material keyword, and
// the fallback creation for project rendergraphs that predate the targets —
// so WorldRenderNode and the world pass stay feature-agnostic: they carry a
// generic extra-attachment list and never learn what a slice stores.

#pragma once

#include "Engine/Rendering/RenderServices.h" // WorldPassTargetsRG
#include "Rendering/Materials/ShaderVariantKey.h" // MaterialKeyword

namespace GameEngine::Engine::Renderer::Pipeline
{

struct ViewDeclare;

class ReflectionsProvider
{
public:
    // The keyword this provider will contribute to the world pass for `view`,
    // decided from view state alone: SSSR active, and a colour target whose
    // sample count the single-sample slices can match.
    //
    // Forward contributors that declare BEFORE WorldRenderNode (terrain) must
    // key their variants with this, and must ask for it here rather than read
    // it back from the world pass: RenderServices' recorded world-pass keywords
    // are reset every frame and written by AddWorldPassForView, and the
    // pipeline's frame resource table — where the *Written names land — is
    // rebuilt at graph-build start. Both are therefore empty at the point an
    // earlier node declares, so either probe answers "no SSSR" every frame.
    static Rendering::MaterialKeyword WorldPassKeyword(ViewDeclare& d);

    // Resolve — or create and publish, when the blueprint doesn't declare
    // them — the SSR G-buffer slices for this view, and append them to
    // `targets` as world-pass MRT attachments. Returns the material keyword
    // the world pass must enable so the forward adapter writes the slices,
    // or None when nothing wants them for the view or the color target is
    // multisampled with no consumer target to resolve into (the SSR slices
    // themselves are single-sample).
    // The slices are wanted when SSSR is active, or when a consumer hands in
    // `normalTarget`: its own single-sample render target for the normal
    // slice (a feature that keeps a per-view shading-normal history), which
    // then replaces the blueprint's or the provider's own texture as the
    // attachment — under MSAA as the resolve target of a transient
    // multisampled slice.
    // On the success path this also publishes Names::View::*Written — the
    // positive "slices are attached and written this frame" signal the SSR
    // node requires; every bail path leaves it absent, which disables the
    // node for the view.
    static Rendering::MaterialKeyword ContributeWorldPassTargets(
        ViewDeclare& d, RenderServices::WorldPassTargetsRG& targets,
        Rendering::RenderGraph::RGTexture normalTarget);

    // Attach the slices the world pass wrote this frame to a later forward
    // pass (`targets`) whose surfaces are in its depth buffer and own the
    // pixels they cover (the water surface), loaded and stored, and return
    // the keyword that makes its variants write them; None when the world
    // pass did not write them for screen-space reflections or when the pass
    // has no depth attachment. Occluded fragments fail the depth test and
    // leave the slices of the surface in front as they are.
    static Rendering::MaterialKeyword ContributeLateForwardPassTargets(
        ViewDeclare& d, RenderServices::WorldPassTargetsRG& targets);

    // Attach the slices the world pass wrote this frame to the transmissive
    // pass (`targets`), loaded and stored, and return the keyword that makes
    // the glass variant write them; None when the world pass did not write
    // them for screen-space reflections, when the pass has no depth attachment,
    // or when the device lacks independent blend (supportsIndependentBlend). Glass draws over the opaque surface
    // the slices describe and is not in the depth buffer, so reflections would
    // replace that surface's probe lobe on a pixel that shows it only through
    // the glass. The glass writes a zero weight, which keeps the pass off its
    // pixels; the other two slices keep the opaque surface's values for their
    // other readers (the DDGI glossy resolve), through the write masks
    // ApplyTransmissivePassWriteMasks sets. Call it only when the pass binds
    // the scene-colour grab: that keyword is how the variant cache tells the
    // transmissive pass's glass from glass the world pass draws.
    static Rendering::MaterialKeyword ContributeTransmissivePassTargets(
        ViewDeclare& d, RenderServices::WorldPassTargetsRG& targets);

    // The colour-blend write masks of a forward variant drawn with
    // `passKeywords`. The transmissive pass's glass (the reflection keyword
    // with the scene-colour grab) writes the weight slice's RGB and nothing of
    // the normal and radiance slices; every other variant is left as built.
    static void ApplyTransmissivePassWriteMasks(Rendering::MaterialKeyword passKeywords,
                                                Rendering::ColorBlendState& blend);
};

} // namespace GameEngine::Engine::Renderer::Pipeline
