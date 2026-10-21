#include "SceneView/SceneViewAutoExposure.h"

#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/PostProcessSettings.h"

namespace GameEngine::Editor
{

void ApplySceneViewAutoExposure(const SceneViewAutoExposure& autoExposure,
                                Engine::Renderer::PostProcessSettings& settings)
{
    settings.AutoExposureActive = true;
    settings.AutoExposureMeter = static_cast<int32>(autoExposure.Meter);
    settings.AutoExposureMinEv = autoExposure.MinEv;
    settings.AutoExposureMaxEv = autoExposure.MaxEv;
    settings.AutoExposureCompensation = Rendering::SampleExposureCompensation(autoExposure.CompensationCurve);
    Rendering::NarrowAutoExposureEnvelope(settings.AutoExposureMinEv, settings.AutoExposureMaxEv,
                                          settings.ExposureClampMinEv, settings.ExposureClampMinWeight,
                                          settings.ExposureClampMaxEv, settings.ExposureClampMaxWeight);
    settings.AutoExposureBiasEv += autoExposure.CompensationEv;
    if (!autoExposure.AdaptationDelay)
    {
        settings.AutoExposureSpeedUp = kInstantAdaptationSpeed;
        settings.AutoExposureSpeedDown = kInstantAdaptationSpeed;
    }
}

} // namespace GameEngine::Editor
