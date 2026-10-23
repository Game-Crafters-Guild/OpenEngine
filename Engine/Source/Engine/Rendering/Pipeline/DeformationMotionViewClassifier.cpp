#include "DeformationMotionViewClassifier.h"

namespace GameEngine::Engine::Renderer
{

DeformationMotionRefusal ClassifyDeformationMotionView(const DeformationMotionViewState& view)
{
    // Nothing to produce comes first: it is the ordinary state of most views,
    // and a view with nothing deforming must not warn about its shape.
    if (!view.HasDeformingBatchKeys)
        return DeformationMotionRefusal::NoDeformingMaterial;
    if (view.CompatibilityProfile)
        return DeformationMotionRefusal::CompatibilityProfile;
    if (view.Letterboxed)
        return DeformationMotionRefusal::Letterboxed;
    if (view.DepthSampleCount != 1u)
        return DeformationMotionRefusal::Multisampled;
    if (!view.HasRenderedHistory)
        return DeformationMotionRefusal::NoRenderedHistory;
    return DeformationMotionRefusal::None;
}

const char* DeformationMotionRefusalReason(DeformationMotionRefusal refusal)
{
    switch (refusal)
    {
    case DeformationMotionRefusal::CompatibilityProfile:
        return "the renderer runs the compatibility profile, whose cooked materials have no motion "
               "variant";
    case DeformationMotionRefusal::Letterboxed:
        return "the view keeps a fixed aspect ratio inside its target. Let it fill the target to "
               "get exact motion";
    case DeformationMotionRefusal::Multisampled:
        return "the view renders multisampled depth, and the motion payload has no correct "
               "multisample resolve. Use TAA instead of MSAA on this view to get it";
    case DeformationMotionRefusal::NoDeformingMaterial:
        return "no material in this view moves its vertices in a form the motion pass can follow "
               "(a plain ModifyVertex that returns a position). Terrain, ocean and grass are not "
               "covered";
    case DeformationMotionRefusal::NoRenderedHistory:
        return "the view's per-object history is not ready on its first frame";
    case DeformationMotionRefusal::None:
        break;
    }
    return "nothing refused it";
}

} // namespace GameEngine::Engine::Renderer
