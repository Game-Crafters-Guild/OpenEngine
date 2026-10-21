#pragma once

#include "Components/Rendering/ExposureCompensationCurve.h"
#include "Components/Rendering/PostProcessVolume.h" // ExposureMeter
#include "Editor/Settings/SceneViewSettings.h"

namespace GameEngine::Engine::Renderer
{
struct PostProcessSettings;
}

namespace GameEngine::Editor
{

// The adaptation speed (stops per second) that stands for "apply each reading at once": the resolve's
// smoothing factor 1 - exp2(-dt * speed) is exactly 1.0 in float32 for any frame time above 1e-7 s, so
// the exposure equals the clamped target on the first frame a reading changes. It needs no shader state.
inline constexpr float kInstantAdaptationSpeed = 1.0e9f;

// The Scene View's Auto settings, as SceneViewSettings holds them; the defaults are the shipped ones.
struct SceneViewAutoExposure
{
    Components::ExposureMeter Meter = SceneViewSettings::kDefaultExposureMeter;
    float MinEv = SceneViewSettings::kDefaultAutoExposureMinEv;
    float MaxEv = SceneViewSettings::kDefaultAutoExposureMaxEv;
    float CompensationEv = 0.0f;
    bool AdaptationDelay = SceneViewSettings::kDefaultAutoExposureAdaptationDelay;
    // The scene's main camera's curve, or the engine default when the scene has no camera, so a curve
    // an author tunes on the camera shows in the Scene View too.
    Math::Curve CompensationCurve = Components::DefaultExposureCompensationCurve();
};

/// Stamps the Scene View's Auto exposure onto the view's blended settings, mirroring the camera-sensor
/// resolve (the engine extraction's ApplyCameraExposure): metering on with the view's meter, its
/// compensation curve, its EV
/// envelope narrowed by any volume Exposure Adjustment clamps, its compensation added to the post-clamp
/// bias (which already holds the volume compensation), and, without the delay, both speeds at
/// kInstantAdaptationSpeed. With the delay the speeds stay the world sensor's.
void ApplySceneViewAutoExposure(const SceneViewAutoExposure& autoExposure,
                                Engine::Renderer::PostProcessSettings& settings);

} // namespace GameEngine::Editor
