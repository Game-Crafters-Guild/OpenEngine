#pragma once

#include "Types/Types.h"

namespace GameEngine::Rendering
{
struct SkySettings;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer {

// std140 layout of the HeightFogParams UBO: filled by HeightFogParamsUploadNode, mirrored field for
// field by height_fog_params_fields.glsl.
struct HeightFogParamsUBO
{
    float intensity = 0.0f;
    float density = 0.0f;
    float minDistance = 0.0f;
    float smoothLength = 1.0f;
    float baseHeight = 0.0f;
    float transitionLength = 120.0f;
    float maxDistance = 1000.0f;
    float gradientStrength = 0.0f;
    float emissiveR = 0.48f;
    float emissiveG = 0.54f;
    float emissiveB = 0.60f;
    float sunIntensity = 1.0f;
    float sunDirX = 0.35f;
    float sunDirY = -0.65f;
    float sunDirZ = 0.68f;
    float phase = -0.5f;
    float sunColorR = 1.0f;
    float sunColorG = 0.82f;
    float sunColorB = 0.58f;
    float phaseWeight0 = 1.0f;
    float phaseWeight1 = 0.0f;
    float skyPower = 1.0f;
    float skyFillStart = 0.0f;
    float skyFillEnd = 1.0f;
    float axisX = 0.0f;
    float axisY = 1.0f;
    float axisZ = 0.0f;
    float noiseScale = 80.0f;
    float gradientLowR = 0.48f;
    float gradientLowG = 0.54f;
    float gradientLowB = 0.60f;
    float noiseStrength = 0.0f;
    float gradientHighR = 0.72f;
    float gradientHighG = 0.78f;
    float gradientHighB = 0.85f;
    float noiseContrast = 1.25f;
    float noiseVelX = 0.0f;
    float noiseVelY = 0.0f;
    float noiseVelZ = 0.0f;
    float noiseTime = 0.0f;
    int32 flags = 0;
    int32 axisMode = 0;
    int32 gradientMode = 0;
    float maxOpacity = 0.85f;
    int32 layerMode = 0;
    float horizonHeightOffset = 0.0f;
    float horizonHeightBlendStart = 0.0f;
    float horizonHeightBlendEnd = 0.0f;
    float noiseMin = 0.0f;
    float noiseMax = 1.0f;
    float noiseFadeStart = 0.0f;
    float noiseFadeEnd = 0.0f;
    float skyHorizonOffset = 0.0f;
    float skyBottomStrength = 0.0f;
    // HeightFogColorSource: where the fog colour comes from this frame.
    int32 colorSource = 0;
    // The sky's exposure trim (2^exposureEV), times the HDRI intensity for an HDRI backdrop.
    float skyHorizonScale = 1.0f;
    // The sky's night blend, for the night composite at the horizon (backdrop source only).
    float skyNightAmount = 0.0f;
    // Added to the view azimuth before the backdrop tap, in radians: the HDRI's rotation.
    float skyAzimuthOffset = 0.0f;
    // Backdrop source: the night sky's horizon colour. Constant source: the horizon radiance before
    // skyHorizonScale.
    float skyHorizonColorR = 0.0f;
    float skyHorizonColorG = 0.0f;
    float skyHorizonColorB = 0.0f;
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
    float _pad2 = 0.0f;
};

static_assert(sizeof(HeightFogParamsUBO) == 256, "HeightFogParamsUBO must be 256 bytes");

// Values of HeightFogParamsUBO::colorSource. The sky values are sky_composite.glsl's
// GE_SKY_HORIZON_SOURCE_* values, which the fog term (height_fog_term.glsl) hands to
// GE_SkyHorizonColor.
enum class HeightFogColorSource : int32
{
    // Emissive, the gradient and the sun in-scatter term.
    Fixed = 0,
    // The sky-view LUT's horizon row at the view azimuth, under the night composite.
    SkyViewLut = 1,
    // skyHorizonColor, already the sky's horizon radiance (gradient sky, stylized ground rim).
    SkyConstant = 2,
    // The HDRI's horizon row at the view azimuth, averaged over a 64th of a turn.
    SkyHdri = 3,
};

enum HeightFogFlags : int32
{
    HeightFogFlagDistanceEnabled = 1 << 0,
    HeightFogFlagHeightEnabled = 1 << 1,
    HeightFogFlagNoiseEnabled = 1 << 2,
    HeightFogFlagSkyEnabled = 1 << 3,
};

void FillHeightFogParamsUBO(const struct PostProcessSettings& settings, HeightFogParamsUBO& out);

// Fills colorSource and the sky colour fields: in the sky horizon colour mode, the source the sky
// draws its horizon from and the terms it draws it with, so the fog's colour is the sky's at the
// horizon. activeSky is the frame's sky, null without one; then, and in the fixed colour mode, the
// fog keeps its fixed colour.
void FillHeightFogSkyColor(const struct PostProcessSettings& settings,
                           const Rendering::SkySettings* activeSky, HeightFogParamsUBO& out);

} // namespace GameEngine::Engine::Renderer
