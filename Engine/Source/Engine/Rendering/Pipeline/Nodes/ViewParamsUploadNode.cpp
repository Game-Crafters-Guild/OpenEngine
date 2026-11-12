#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"

#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"

#include "Core/Time.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/ViewParamsLayout.h"
#include "Rendering/Passes/TonemapPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

float ViewParamsUploadNode::ComputeTaauMipBias(uint32_t renderWidth, uint32_t outputWidth)
{
    if (renderWidth == 0 || outputWidth == 0 || renderWidth >= outputWidth)
        return 0.0f;
    const float bias =
        std::log2(static_cast<float>(renderWidth) / static_cast<float>(outputWidth));
    // Two mip levels is the floor, and the pre-pass does reach it: render scale
    // clamps to [kMinRenderScale, 1.0] with kMinRenderScale = 0.25, whose exact
    // ratio already lands on -2.0, and the internal extent is an even-snapped
    // FLOOR of output*scale, so the realised ratio can sit below the requested
    // scale and the clamp engages. Dynamic resolution cannot get here — its own
    // MinScale is separately clamped to >= 0.5.
    return std::max(bias, -2.0f);
}

bool ViewParamsUploadNode::Initialize(std::string nodeId, std::string nodeJson, std::string* outError)
{
    m_Id = std::move(nodeId);
    m_Json = std::move(nodeJson);

    try
    {
        auto j = nlohmann::json::parse(m_Json);
        if (j.is_object())
        {
            if (j.contains("buffer") && j["buffer"].is_string())
            {
                m_BufferRef = j["buffer"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = std::string("JSON parse failed: ") + e.what();
        return false;
    }

    return true;
}

void ViewParamsUploadNode::DeclareForView(ViewDeclare& d)
{
    // Dissolved into the upload ring: payload computed and written at
    // declaration — no pass, no graph declaration (host-coherent). Sized
    // from the STRUCT (sizeof(ViewParamsUBO), asserted in ViewParamsLayout.h); the
    // blueprint's size entry is not consulted on this Upload path.
    const auto* vd = d.Services.Views().FindViewDesc(d.View.id);
    if (!vd || vd->cameraId == 0)
        return;
    const auto* stored = d.Services.Views().FindCameraData(vd->cameraId);
    if (!stored)
        return;
    auto alloc = d.Frame.AllocUpload<ViewParamsUBO>();
    if (!alloc.Valid())
        return;

    // Raster domain: ViewParams describes the raster that produced the depth
    // its consumers (GTAO, fog apply, clouds, DoF) reconstruct against, so a
    // TAA view uploads the jittered proj/viewProj here (no-op when TAA is
    // off). The registry's stored CameraData stays unjittered (logic domain).
    const auto* tempAA = d.Services.Views().FindViewAntiAliasing(d.View.id);
    Rendering::CameraData jittered = *stored;
    if (tempAA != nullptr)
    {
        jittered = d.Services.Views().ResolveJitteredCameraData(
            d.View.id, d.Frame.FrameIndex(), d.RenderWidth, d.RenderHeight);
    }
    const Rendering::CameraData* cam = &jittered;

    GameEngine::Rendering::Matrix4x4 projM = cam->proj;
    GameEngine::Rendering::Matrix4x4 invProjM = GameEngine::Mathematics::Inverse(projM);

    ViewParamsUBO ubo{};
    std::memcpy(ubo.ge_invProj, invProjM.Data(), sizeof(ubo.ge_invProj));
    std::memcpy(ubo.ge_view, cam->view.Data(), sizeof(ubo.ge_view));
    const ProjectionDepthTerms depthTerms = ProjectionDepthTerms::Of(cam->proj);
    ubo.ge_depthTerms[0] = depthTerms.M22;
    ubo.ge_depthTerms[1] = depthTerms.M32;
    ubo.ge_depthTerms[2] = depthTerms.M23;
    ubo.ge_depthTerms[3] = depthTerms.M33;
    // Near is the linearized depth at raw 1; the range is the carried culling range.
    const float zn = LinearizeReverseZDepthLH_ZO(1.0f, depthTerms);
    const float range = d.Services.Views().ResolveCameraRanges(d.View.id).CullingRange;
    const float logRatio = std::log(range / std::max(zn, 1e-4f));
    ubo.ge_viewRanges[0] = zn;
    ubo.ge_viewRanges[1] = range;
    ubo.ge_viewRanges[2] = (logRatio > 1e-6f) ? (1.0f / logRatio) : 0.0f;
    ubo.ge_viewRanges[3] = d.Services.GetShaderAnimationTimeSeconds();

    GameEngine::Rendering::Matrix4x4 viewM = cam->view;
    GameEngine::Rendering::Matrix4x4 invViewM = GameEngine::Mathematics::Inverse(viewM);
    const float* iv = invViewM.Data();
    ubo.ge_cameraPosWS[0] = iv[12];
    ubo.ge_cameraPosWS[1] = iv[13];
    ubo.ge_cameraPosWS[2] = iv[14];
    ubo.ge_cameraPosWS[3] = 0.0f;
    // view->world, so consumers reconstruct a world position from a view-space
    // point without a per-pixel matrix inverse (SSR reprojection).
    std::memcpy(ubo.ge_invView, invViewM.Data(), sizeof(ubo.ge_invView));

    // The relative frame takes the same arithmetic on the origin-relative view, so in sector
    // (0,0,0), where viewRel is a copy of view, each field holds its absolute partner's bits.
    const GameEngine::Rendering::Matrix4x4 invViewRelM = GameEngine::Mathematics::Inverse(cam->viewRel);
    std::memcpy(ubo.ge_viewRel, cam->viewRel.Data(), sizeof(ubo.ge_viewRel));
    std::memcpy(ubo.ge_invViewRel, invViewRelM.Data(), sizeof(ubo.ge_invViewRel));
    ubo.ge_cameraPosRel[0] = invViewRelM[3][0];
    ubo.ge_cameraPosRel[1] = invViewRelM[3][1];
    ubo.ge_cameraPosRel[2] = invViewRelM[3][2];
    ubo.ge_cameraPosRel[3] = 0.0f;
    for (int32 axis = 0; axis < 4; ++axis)
        ubo.ge_renderOrigin[axis] = static_cast<float>(cam->renderOriginSector[axis]);

    // THE view extent — the same numbers buffer-size expressions and
    // dispatch sizing use.
    const float sw = std::max(static_cast<float>(d.RenderWidth), 1.0f);
    const float sh = std::max(static_cast<float>(d.RenderHeight), 1.0f);
    ubo.ge_screenSize[0] = sw;
    ubo.ge_screenSize[1] = sh;
    ubo.ge_screenSize[2] = 1.0f / sw;
    ubo.ge_screenSize[3] = 1.0f / sh;
    std::memcpy(ubo.ge_viewProj, cam->viewProj.Data(), sizeof(ubo.ge_viewProj));
    std::memcpy(ubo.ge_proj, cam->proj.Data(), sizeof(ubo.ge_proj)); // ge_proj == the projection ge_invProj inverts

    // TAAU texture-sharpness compensation: material sampling adds this bias per
    // sample (adapter_forward's GE_MaterialTexture texture() overload) so texture
    // detail at the reduced internal raster matches display resolution. Derived
    // from the ACTUAL extent ratio (even-snap included), so it tracks the slider
    // live; exactly 0.0 when the view is not upscaling — the scale-1.0
    // byte-neutrality gate.
    ubo.ge_mipBiasParams[0] = ComputeTaauMipBias(d.RenderWidth, d.OutputWidth);

    // Previous frame's UNJITTERED world->clip. This history is independent of
    // the active AA mode, so SSSR's temporal lane also works with AA off/MSAA.
    bool previousValid = false;
    const ViewDeformationClock clock =
        d.Services.TemporalHistory().ResolveDeformationClock(Time::GetCumulativeSeconds());
    const ViewTemporalSample currentSample{*stored, clock.TimeSeconds,
                                           d.Services.GetScrollAnimationTimeSeconds(), clock.Origin};
    // A declare path: the rotation takes the frame stream's submitted-frame
    // count so a declared-and-abandoned frame never becomes a later previous.
    const ViewTemporalSample* previous = d.Services.TemporalHistory().Advance(
        d.View.id, d.Frame.FrameIndex(), currentSample, d.Frame.SubmittedFrameCount(),
        &previousValid);
    const Rendering::CameraData& prevCamera = (previousValid && previous) ? previous->Camera : *stored;
    std::memcpy(ubo.ge_prevViewProj, prevCamera.viewProj.Data(), sizeof(ubo.ge_prevViewProj));
    // The previous relative view-projection takes positions relative to the previous frame's
    // origin: move it into this frame's sector.
    std::memcpy(ubo.ge_prevViewProjRel, prevCamera.viewProjRel.Data(), sizeof(ubo.ge_prevViewProjRel));
    if (!std::equal(prevCamera.renderOriginSector, prevCamera.renderOriginSector + 3, stored->renderOriginSector))
    {
        RebaseTranslationColumnBySectorDelta(prevCamera.viewProjRel.Data(), prevCamera.renderOriginSector,
                                             stored->renderOriginSector, ubo.ge_prevViewProjRel);
    }

    // Sub-pixel NDC jitter, current (xy) and previous (zw) frame. A consumer
    // reprojecting through the UNJITTERED ge_prevViewProj adds zw to land on the
    // previous frame's actual raster grid (ApplyNdcJitter is additive in NDC).
    // Left zero when no jittered AA drives this view.
    if (tempAA != nullptr && tempAA->Enabled)
    {
        ubo.ge_taaJitter[0] = tempAA->NdcJitterX;
        ubo.ge_taaJitter[1] = tempAA->NdcJitterY;
        ubo.ge_taaJitter[2] = tempAA->PrevNdcJitterX;
        ubo.ge_taaJitter[3] = tempAA->PrevNdcJitterY;
    }

    // The exposure and the look the tonemap applies to this view, from the effective settings the
    // AutoExposure and Tonemap nodes read. While auto exposure meters the view, shaders read the metered
    // scale from ExposureHistory (GE_ViewExposureScale). The view's no-look output overrides the blended look.
    const PostProcessSettings& post = d.Services.GetEffectivePostProcessSettings(d.View.id, d.View.worldId);
    ubo.ge_exposureParams[0] = post.Exposure;
    ubo.ge_exposureParams[1] = post.IsAutoExposureActive() ? 1.0f : 0.0f;
    ubo.ge_exposureParams[2] = static_cast<float>(
        post.ViewOutput == PostProcessViewOutput::NoLook ? Passes::kTonemapNoLookArm : post.Look);

    *alloc.Ptr = ubo;
    d.PublishBuffer(m_BufferRef,
                    {alloc.Buffer, alloc.Offset, sizeof(ViewParamsUBO), /*Graph*/ {}});
}

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
