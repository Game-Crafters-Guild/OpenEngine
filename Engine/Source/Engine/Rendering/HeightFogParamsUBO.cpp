#include "Engine/Rendering/HeightFogParamsUBO.h"

#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/SkyBackdrop.h"
#include "Rendering/Sky/SkySettings.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer {

void FillHeightFogParamsUBO(const PostProcessSettings& settings, HeightFogParamsUBO& out)
{
    out.intensity = settings.HeightFogIntensity;
    out.density = settings.HeightFogDensity;
    out.maxOpacity = settings.HeightFogMaxOpacity;
    out.minDistance = settings.HeightFogMinDistance;
    out.smoothLength = settings.HeightFogSmoothLength;
    out.baseHeight = settings.HeightFogBaseHeight;
    out.transitionLength = settings.HeightFogTransitionLength;
    out.maxDistance = settings.HeightFogMaxDistance;
    out.gradientStrength = settings.HeightFogGradientStrength;
    out.emissiveR = settings.HeightFogEmissiveR;
    out.emissiveG = settings.HeightFogEmissiveG;
    out.emissiveB = settings.HeightFogEmissiveB;
    out.sunIntensity = settings.HeightFogSunIntensity;
    out.sunDirX = settings.HeightFogSunDirX;
    out.sunDirY = settings.HeightFogSunDirY;
    out.sunDirZ = settings.HeightFogSunDirZ;
    out.phase = settings.HeightFogPhase;
    out.sunColorR = settings.HeightFogSunColorR;
    out.sunColorG = settings.HeightFogSunColorG;
    out.sunColorB = settings.HeightFogSunColorB;
    out.phaseWeight0 = settings.HeightFogPhaseWeight0;
    out.phaseWeight1 = settings.HeightFogPhaseWeight1;
    out.skyPower = settings.HeightFogSkyPower;
    out.skyFillStart = settings.HeightFogSkyFillStart;
    out.skyFillEnd = settings.HeightFogSkyFillEnd;
    out.axisX = settings.HeightFogAxisX;
    out.axisY = settings.HeightFogAxisY;
    out.axisZ = settings.HeightFogAxisZ;
    out.noiseScale = settings.HeightFogNoiseScale;
    out.gradientLowR = settings.HeightFogGradientLowR;
    out.gradientLowG = settings.HeightFogGradientLowG;
    out.gradientLowB = settings.HeightFogGradientLowB;
    out.noiseStrength = settings.HeightFogNoiseStrength;
    out.gradientHighR = settings.HeightFogGradientHighR;
    out.gradientHighG = settings.HeightFogGradientHighG;
    out.gradientHighB = settings.HeightFogGradientHighB;
    out.noiseContrast = settings.HeightFogNoiseContrast;
    out.noiseVelX = settings.HeightFogNoiseVelX;
    out.noiseVelY = settings.HeightFogNoiseVelY;
    out.noiseVelZ = settings.HeightFogNoiseVelZ;

    out.flags = 0;
    if (settings.HeightFogDistanceFogEnabled)
        out.flags |= HeightFogFlagDistanceEnabled;
    if (settings.HeightFogHeightFogEnabled)
        out.flags |= HeightFogFlagHeightEnabled;
    if (settings.HeightFogNoiseEnabled)
        out.flags |= HeightFogFlagNoiseEnabled;
    if (settings.HeightFogSkyEnabled)
        out.flags |= HeightFogFlagSkyEnabled;

    out.axisMode = settings.HeightFogAxisMode;
    out.gradientMode = settings.HeightFogGradientMode;
    out.layerMode = settings.HeightFogLayerMode;
    out.horizonHeightOffset = settings.HeightFogHorizonHeightOffset;
    out.horizonHeightBlendStart = settings.HeightFogHorizonHeightBlendStart;
    out.horizonHeightBlendEnd = settings.HeightFogHorizonHeightBlendEnd;
    out.noiseMin = settings.HeightFogNoiseMin;
    out.noiseMax = settings.HeightFogNoiseMax;
    out.noiseFadeStart = settings.HeightFogNoiseFadeStart;
    out.noiseFadeEnd = settings.HeightFogNoiseFadeEnd;
    out.skyHorizonOffset = settings.HeightFogSkyHorizonOffset;
    out.skyBottomStrength = settings.HeightFogSkyBottomStrength;
}

void FillHeightFogSkyColor(const PostProcessSettings& settings, const Rendering::SkySettings* activeSky,
                           HeightFogParamsUBO& out)
{
    out.colorSource = static_cast<int32>(HeightFogColorSource::Fixed);
    if (settings.HeightFogColorMode != static_cast<int32>(::GameEngine::Components::HeightFogColorMode::SkyHorizon) ||
        !activeSky)
        return;

    const Rendering::SkySettings& sky = *activeSky;
    // sky_render.frag's single exposure multiply, applied to every backdrop kind.
    const float exposure = std::exp2(sky.exposureEV);
    out.skyHorizonScale = exposure;
    out.skyNightAmount = 0.0f;
    out.skyAzimuthOffset = 0.0f;

    switch (ClassifySkyBackdrop(sky))
    {
    case SkyBackdropKind::Hdri:
        out.colorSource = static_cast<int32>(HeightFogColorSource::SkyHdri);
        out.skyHorizonScale = exposure * sky.hdriIntensity;
        out.skyAzimuthOffset = sky.skyboxRotationRadians;
        return;
    case SkyBackdropKind::Gradient:
        // The gradient at view y == 0 is its horizon colour alone.
        out.colorSource = static_cast<int32>(HeightFogColorSource::SkyConstant);
        out.skyHorizonColorR = sky.gradientSkyHorizonColor[0];
        out.skyHorizonColorG = sky.gradientSkyHorizonColor[1];
        out.skyHorizonColorB = sky.gradientSkyHorizonColor[2];
        return;
    case SkyBackdropKind::Atmosphere:
        break;
    }

    const float nightAmount = std::clamp(sky.nightSkyBlend, 0.0f, 1.0f);
    if (sky.belowHorizonMode == static_cast<uint32_t>(Rendering::SkyBelowHorizonMode::StylizedGround))
    {
        // sky_render.frag paints the stylized rim over the horizon, fully at viewZenithCos == 0.
        out.colorSource = static_cast<int32>(HeightFogColorSource::SkyConstant);
        out.skyHorizonColorR = std::lerp(sky.groundHorizonColor[0], sky.groundHorizonNightColor[0], nightAmount);
        out.skyHorizonColorG = std::lerp(sky.groundHorizonColor[1], sky.groundHorizonNightColor[1], nightAmount);
        out.skyHorizonColorB = std::lerp(sky.groundHorizonColor[2], sky.groundHorizonNightColor[2], nightAmount);
        return;
    }

    out.colorSource = static_cast<int32>(HeightFogColorSource::SkyViewLut);
    out.skyNightAmount = nightAmount;
    out.skyHorizonColorR = sky.nightSkyHorizonColor[0];
    out.skyHorizonColorG = sky.nightSkyHorizonColor[1];
    out.skyHorizonColorB = sky.nightSkyHorizonColor[2];
}

} // namespace GameEngine::Engine::Renderer
