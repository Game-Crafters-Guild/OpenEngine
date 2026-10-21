#include "SceneView/SceneViewPostProcessSwitches.h"

#include "Components/Rendering/Look.h"
#include "Engine/Rendering/PostProcessSettings.h"

namespace GameEngine::Editor
{

namespace
{
void SwitchTonemappingOff(bool hdrOutputActive, Engine::Renderer::PostProcessSettings& settings)
{
    if (hdrOutputActive)
        settings.ViewOutput = Engine::Renderer::PostProcessViewOutput::NoLook;
    else
        settings.Look = static_cast<int32_t>(Components::Look::Neutral);
}
} // namespace

void ApplySceneViewPostProcessSwitches(const SceneViewPostProcessSwitches& switches,
                                       bool hdrOutputActive,
                                       Engine::Renderer::PostProcessSettings& settings)
{
    if (!switches.PostProcessing)
    {
        settings.DisableBloom();
        settings.ColorFilterIntensity = 0.0f;
        settings.CasStrength = 0.0f;
        settings.CrtIntensity = 0.0f;
        settings.CrtExposureCompensation = 0.0f;
        settings.VhsIntensity = 0.0f;
        settings.HeightFogIntensity = 0.0f;
        settings.HeightFogDensity = 0.0f;
        settings.VolumetricFogIntensity = 0.0f;
        settings.VolumetricFogTemporalEnabled = 0;
        settings.VolumetricFogTemporalBlend = 0.0f;
        SwitchTonemappingOff(hdrOutputActive, settings);
        return;
    }
    if (!switches.Bloom)
        settings.DisableBloom();
    if (!switches.Tonemap)
        SwitchTonemappingOff(hdrOutputActive, settings);
    if (!switches.ColorFilter)
        settings.ColorFilterIntensity = 0.0f;
    if (!switches.ContrastAdaptiveSharpening)
        settings.CasStrength = 0.0f;
    if (!switches.Crt)
    {
        settings.CrtIntensity = 0.0f;
        settings.CrtExposureCompensation = 0.0f;
    }
}

} // namespace GameEngine::Editor
