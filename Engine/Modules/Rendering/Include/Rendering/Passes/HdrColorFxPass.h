#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Utils/CubeLutGpuBinding.h"

namespace GameEngine {
namespace Rendering {

class IDevice;

namespace Passes {

// An offscreen run of the grade pass (hdr_color_fx.frag) with only its last stage, the Cube LUT at
// the Grade stage, active: white balance, the color filter, the three-way corrector and the hue
// shift stay at identity. The pipeline's HDRColorFx node runs the same shader on the frame; this
// declares it on a caller's scene-linear image (a LUT preview at the Grade stage, which a following
// AddTonemapPassRG renders through the look).
struct HdrColorFxParams
{
	// The static exposure the grade anchors at (hdr_color_fx.frag multiplies it in before the
	// stages and divides it back out, so the output stays scene-linear).
	float exposure = 1.0f;
	// The uploaded tables with their input encoding (TextureService::TryGetCubeLutGpuBinding) and
	// how much of the LUT's output to keep (0 = none: the pass then copies its input).
	CubeLutGpuBindingState lut{};
	float lutIntensity = 0.0f;
};

// Declares the pass from src to dst, which must differ. Returns an invalid pass (declares nothing)
// on a missing shader package (the loader retries next frame) or src == dst.
RenderGraph::RGPass AddHdrColorFxPassRG(RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, RenderGraph::RGTexture dst,
                                      const HdrColorFxParams& params, const char* passName = "HdrColorFx");

// Release the per-device resources the pass created. Safe to call more than once or with null.
void CleanupHdrColorFxPassForDevice(IDevice* device);

} // namespace Passes
} // namespace Rendering
} // namespace GameEngine
