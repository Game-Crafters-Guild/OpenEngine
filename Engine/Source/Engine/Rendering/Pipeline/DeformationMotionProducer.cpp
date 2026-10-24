#include "Engine/Rendering/Pipeline/DeformationMotionProducer.h"

#include "DeformationMotionViewClassifier.h"
#include "Engine/Rendering/DeformationMotionParams.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewTemporalHistory.h"
#include "Logger/Logger.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline
{
using SlicePhase = ::GameEngine::Rendering::GPUDrawStreamBuilder::SlicePhase;

namespace
{
std::string ViewLabel(const Rendering::ViewDesc& view)
{
    const std::string id = "view " + std::to_string(static_cast<uint32_t>(view.id));
    if (view.debugName == nullptr || view.debugName[0] == '\0')
        return id;
    return std::string("\"") + view.debugName + "\" (" + id + ")";
}

// One line per view each time its state changes, so a line is never left
// standing after the view moved on. "Nothing in the view deforms" logs nothing
// and keeps the last state: it is the ordinary state of most views, and a view
// whose trees leave and re-enter the frame has nothing new to say. A view the
// user can change (letterboxed, multisampled) is a warning; the expected
// states are information.
void LogStateChange(ViewDeclare& view, DeformationMotionRefusal refusal)
{
    if (refusal == DeformationMotionRefusal::NoDeformingMaterial)
        return;
    uint8_t& logged = view.Services.Views().PerView(view.View.id).DeformationMotionLogged;
    if (logged == static_cast<uint8_t>(refusal))
        return;
    logged = static_cast<uint8_t>(refusal);
    const std::string label = ViewLabel(view.View);
    switch (refusal)
    {
    case DeformationMotionRefusal::None:
        Logger::Log::Info("Deformation motion: {} writes exact motion vectors for its deforming meshes.",
                          label);
        break;
    case DeformationMotionRefusal::NoRenderedHistory:
        Logger::Log::Info("Deformation motion: {} writes no exact motion vectors for its deforming "
                          "meshes yet, because {}.",
                          label, DeformationMotionRefusalReason(refusal));
        break;
    case DeformationMotionRefusal::CompatibilityProfile:
        Logger::Log::Info("Deformation motion: {} writes no exact motion vectors for its deforming "
                          "meshes, because {}.",
                          label, DeformationMotionRefusalReason(refusal));
        break;
    default:
        Logger::Log::Warning("Deformation motion: {} writes no exact motion vectors for its deforming "
                             "meshes, because {}. They reproject from the camera alone.",
                             label, DeformationMotionRefusalReason(refusal));
        break;
    }
}
} // namespace

bool DeclareDeformationMotion(ViewDeclare& view, Rendering::RenderGraph::RGTexture target,
                              const ViewTemporalSample* previous, bool previousValid,
                              double currentOrigin)
{
    if (!target.IsValid() || !view.ViewDepth.IsValid())
        return false;

    // A refused view records nothing and declares no pass, and the mover lane
    // owns its target as it does on a view with nothing deforming. The deforming subset is
    // derived alongside the batch keys, so asking costs a lookup.
    auto& drawBuilder = view.Services.GetWorldDrawBuilder();
    auto* streams = view.Services.GetDrawStreamBuilder();
    const uint32_t viewId = static_cast<uint32_t>(view.View.id);
    DeformationMotionViewState state{};
    state.CompatibilityProfile = view.Services.GetProfile().IsCompat();
    state.Letterboxed = view.Services.Views().GetViewLetterbox(view.View.id).active;
    state.DepthSampleCount = view.Frame.Graph().ResourceDesc(view.ViewDepth.Id).SampleCount;
    state.HasDeformingBatchKeys = !drawBuilder.GetDeformingBatchKeys(view.View.id).empty();
    // The motion variant declares the validity binding unconditionally, so a
    // view whose history is not allocated must record nothing rather than draw
    // with an unwritten descriptor.
    state.HasRenderedHistory =
        streams != nullptr && streams->GetRenderedHistoryForRead(viewId).IsValid();
    const DeformationMotionRefusal refusal = ClassifyDeformationMotionView(state);
    LogStateChange(view, refusal);
    if (refusal != DeformationMotionRefusal::None)
        return false;

    DeformationMotionEndpoint endpoint{};
    endpoint.Previous = previous;
    endpoint.PreviousValid = previousValid;
    endpoint.CurrentOrigin = currentOrigin;

    // Phase A owns the clear. The pass returns invalid when the endpoint pair
    // is not differenceable, which is the discontinuity policy's "no draw".
    const auto phaseA = view.Services.AddDeformationMotionPassForView(
        view.Frame, view.View.id, target, view.ViewDepth, endpoint, /*clearTarget=*/true,
        SlicePhase::A);
    if (!phaseA.IsValid())
        return false;

    // The occlusion-recovery generation, consumed symmetrically with the
    // prepass: a deforming instance revealed in phase B is not left on the
    // sentinel. It loads — it completes phase A's image rather than replacing
    // it — and needs no ordering of its own against phase A's writes, because
    // the depth both test against is already the frame's final depth. Declared
    // only when the view published phase-B ranges at all; every non-HZB view
    // publishes none.
    if (streams->HasPublishedRangesForPhase(
            viewId, ::GameEngine::Rendering::GPUDrawStreamBuilder::kCascadeIndexNone, SlicePhase::B))
    {
        view.Services.AddDeformationMotionPassForView(view.Frame, view.View.id, target,
                                                      view.ViewDepth, endpoint,
                                                      /*clearTarget=*/false, SlicePhase::B);
    }
    return true;
}

} // namespace GameEngine::Engine::Renderer::Pipeline
