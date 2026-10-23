#include "Engine/Rendering/DeformationMotionParams.h"

#include "Engine/Rendering/ViewTemporalHistory.h"

#include <cstring>

namespace GameEngine::Engine::Renderer
{

bool BuildDeformationMotionParams(const DeformationMotionEndpoint& endpoint,
                                  const DeformationMotionRaster& raster,
                                  DeformationMotionParamsGPU& out)
{
    if (endpoint.Previous == nullptr || !endpoint.PreviousValid)
        return false;
    // A re-anchored origin (the history's first frame, the precision ceiling)
    // leaves the two endpoints measured from different zeros. Their difference
    // is then not this frame's delta, so the pair is treated as absent history
    // for exactly the one frame the re-anchor costs.
    if (endpoint.Previous->DeformationOrigin != endpoint.CurrentOrigin)
        return false;
    if (raster.Viewport.Width <= 0.0f || raster.Viewport.Height <= 0.0f)
        return false;

    std::memcpy(out.PrevViewProj, endpoint.Previous->Camera.viewProj, sizeof(out.PrevViewProj));
    out.PrevTimeParams[0] = endpoint.Previous->DeformationTimeSeconds;
    out.PrevTimeParams[1] = endpoint.Previous->DeformationScrollSeconds;
    out.PrevTimeParams[2] = 0.0f;
    out.PrevTimeParams[3] = 0.0f;
    out.ViewportRect[0] = raster.Viewport.X;
    out.ViewportRect[1] = raster.Viewport.Y;
    out.ViewportRect[2] = raster.Viewport.Width;
    out.ViewportRect[3] = raster.Viewport.Height;
    // NDC-to-viewport-UV is a halving, and the y axis flips: ApplyNdcJitter
    // adds the offset in Y-up NDC while GE_YUpNdcToViewportUV flips y. The
    // fragment subtracts this from the UV it reads off gl_FragCoord, which
    // recovers the unjittered projection at the pixel centre.
    out.JitterUv[0] = 0.5f * raster.NdcJitterX;
    out.JitterUv[1] = -0.5f * raster.NdcJitterY;
    out.JitterUv[2] = 0.0f;
    out.JitterUv[3] = 0.0f;
    return true;
}

} // namespace GameEngine::Engine::Renderer
