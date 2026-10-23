// The deforming-motion producer's declaration policy.
//
// The producer is a pass of its own that re-rasterizes the view's deforming
// ranges into the shared motion target under read-only camera depth. It and
// the mover lane write one target, so the decision of WHO CLEARS belongs to
// one place: the producer, when it runs, owns the clear and the mover writes
// that follow load.
//
// This is the policy half: whether this view can produce anything at all this
// frame, and which culling generations to record. The pass itself is declared
// by RenderServices::AddDeformationMotionPassForView.
#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::Engine::Renderer
{
struct ViewTemporalSample;
}

namespace GameEngine::Engine::Renderer::Pipeline
{

/// Declare the deforming-motion production for `view` into `target`.
///
/// Returns true when the producer recorded into the target and therefore
/// owns its clear; false when nothing was recorded, in which case the caller's
/// own writes keep the clear and the frame is unchanged from the no-producer
/// one. False is the answer for: a refused view (DeformationMotionRefusal,
/// which includes no deforming material in the view), and no differenceable
/// previous endpoint (a view's first frame, a
/// device rebuild, an origin re-anchor) — the last of which is the
/// discontinuity policy, and it leaves the sentinel rather than a zero vector.
///
/// `previous` and `previousValid` are the rotated history the caller already
/// resolved; `currentOrigin` is the deformation origin this frame's current
/// endpoint was formed against.
bool DeclareDeformationMotion(ViewDeclare& view,
                              Rendering::RenderGraph::RGTexture target,
                              const ViewTemporalSample* previous, bool previousValid,
                              double currentOrigin);

} // namespace GameEngine::Engine::Renderer::Pipeline
