#pragma once

#include "Components/Rendering/LightPhotometry.h" // kReferenceWhiteNits — single exposure/light anchor
#include "Components/Rendering/PostProcessVolume.h" // ExposureMode
#include "Engine/Rendering/ExposureCompensationSamples.h"
#include "Engine/Rendering/PostProcessSettings.h"     // kExposureClampNoOpMinEv / MaxEv
#include "Mathematics/Curve.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Rendering
{

// Absolute photographic EV100 that maps reference white (203 nits == scene-linear 1.0, the same
// LightPhotometry/emission anchor) to display white, i.e. exposureScale == 1.0 (today's look).
// Because physical lights are pre-divided by the 203 anchor, the engine's EV100 IS the real-world
// photographic scale: bright sun ~15, overcast ~12, interior ~7-9, full moon ~-4.
inline const float kNeutralExposureEV = std::log2(Components::kReferenceWhiteNits);

// Auto exposure keys the metered average to 18 % grey: exposed (pre-tonemap) scene-linear 0.18, where
// an 18 % card lands under Manual/Physical exposure at the scene's photographic EV. Compensation (in
// stops) is the one lever that moves it.
inline constexpr float kAutoExposureMiddleGrey = 0.18f;

// Absolute EV100 -> the linear multiplier the tonemap applies. EV100 == kNeutralExposureEV -> 1.0
// (so a brighter scene -> higher EV -> smaller multiplier, the photographic convention).
inline float EvToLinearExposure(float ev100)
{
    return std::exp2(kNeutralExposureEV - ev100);
}

// The inverse: a linear exposure multiplier -> the absolute EV100 it represents.
inline float LinearExposureToEv(float scale)
{
    return kNeutralExposureEV - std::log2(scale > 1e-8f ? scale : 1e-8f);
}

// The scene-linear log2 luminance Auto exposure meters for a scene at `ev100`: an 18 % card under that
// light, log2(kAutoExposureMiddleGrey) + ev100 - kNeutralExposureEV. Both meters' readings are in this
// unit in auto_exposure_resolve.comp, and its key / luminance there is EvToLinearExposure(ev100) before
// the clamp and the compensation, the exposure a Manual camera at ev100 gives.
inline float MeteredLogLuminance(float ev100)
{
    return std::log2(kAutoExposureMiddleGrey) + ev100 - kNeutralExposureEV;
}

// An exposure compensation curve (Components::ExposureCompensationCurve.h: key Time the metered
// EV100, Value the stops) as the resolve reads it: ExposureCompensationSamples::kCount samples from
// the first key's EV100 to the last, through the one curve evaluator. An empty curve samples to zero.
inline Engine::Renderer::ExposureCompensationSamples SampleExposureCompensation(const ::GameEngine::Math::Curve& curve)
{
    using Samples = Engine::Renderer::ExposureCompensationSamples;
    Samples samples{};
    if (curve.KeyCount == 0)
        return samples;
    samples.StartEv = curve.Keys[0].Time;
    samples.EndEv = curve.Keys[curve.KeyCount - 1].Time;
    const float step = (samples.EndEv - samples.StartEv) / static_cast<float>(Samples::kCount - 1);
    for (uint32_t i = 0; i < Samples::kCount; ++i)
        samples.Stops[i] = curve.Evaluate(samples.StartEv + step * static_cast<float>(i));
    return samples;
}

// Absolute photographic EV100 from physical camera settings (APEX): EV100 = log2(N^2 / t * 100/ISO).
// f/16, 1/100 s, ISO 100 (sunny-16) -> ~14.6. Inputs are floored to stay finite for degenerate values.
inline float PhysicalCameraEv100(float aperture, float shutterTime, float iso)
{
    const float n = aperture > 1.0e-3f ? aperture : 1.0e-3f;
    const float t = shutterTime > 1.0e-6f ? shutterTime : 1.0e-6f;
    const float s = iso > 1.0f ? iso : 1.0f;
    return std::log2((n * n) / t * (100.0f / s));
}

// Resolve an authored exposure into the single linear scene multiplier the GPU tonemap consumes.
// Fixed uses the legacy linear `linearExposure`; every other (EV-based) mode uses `ev100` — Manual
// passes its authored EV100, Physical passes PhysicalCameraEv100(). ExposureCompensation is a +/-
// stop trim in all modes (+ brightens). Fixed + Exposure 1.0 + compensation 0 resolves to EXACTLY
// 1.0, so the render graph's "exposure == 1.0" tonemap-skip fast path is preserved for default scenes.
inline float ResolveExposureScale(Components::ExposureMode mode, float linearExposure,
                                  float ev100, float compensationEv)
{
    const float base = (mode == Components::ExposureMode::Fixed)
                           ? linearExposure
                           : EvToLinearExposure(ev100);
    return base * std::exp2(compensationEv);
}

// The EV100 to feed ResolveExposureScale for a given mode (Physical computes from the camera;
// Manual/anything-else uses the authored EV; Fixed is unused but returns the authored EV harmlessly).
inline float ExposureEv100ForMode(Components::ExposureMode mode, float manualEv100,
                                  float aperture, float shutterTime, float iso)
{
    return (mode == Components::ExposureMode::Physical)
               ? PhysicalCameraEv100(aperture, shutterTime, iso)
               : manualEv100;
}

namespace Detail
{
// The clamp the volumes authored, from the blended clamp and its blended weight: the blend starts
// both at the no-op endpoint with weight 0, so the blended clamp is noOp + weight x (authored - noOp),
// with the authored value the clamping volumes' weighted mean.
inline float AuthoredExposureClamp(float blendedClampEv, float weight, float noOpEv)
{
    return noOpEv + (blendedClampEv - noOpEv) / weight;
}
} // namespace Detail

// Intersect an auto-exposure adaptation envelope with narrow-only clamp bounds (e.g. a volume
// ExposureAdjustmentEffect): a clamp can tighten the envelope, never widen it, and the range
// stays non-inverted. Shared by the engine resolve and the editor Scene View override so the
// preview cannot drift from the final render. Each side fades in with its clamp's blend weight: at
// weight w the bound moves w of the way from the envelope's own value to the authored clamp, so a
// view with no volume keeps its envelope (the default floor of -7 reaches a moonless night, below the
// no-op endpoint), and entering a volume's blend region moves the bound smoothly.
inline void NarrowAutoExposureEnvelope(float& minEv, float& maxEv, float clampMinEv, float clampMinWeight,
                                       float clampMaxEv, float clampMaxWeight)
{
    if (clampMinWeight > 0.0f)
    {
        const float authored =
            Detail::AuthoredExposureClamp(clampMinEv, clampMinWeight, Engine::Renderer::kExposureClampNoOpMinEv);
        minEv += std::min(clampMinWeight, 1.0f) * (std::max(minEv, authored) - minEv);
    }
    if (clampMaxWeight > 0.0f)
    {
        const float authored =
            Detail::AuthoredExposureClamp(clampMaxEv, clampMaxWeight, Engine::Renderer::kExposureClampNoOpMaxEv);
        maxEv += std::min(clampMaxWeight, 1.0f) * (std::min(maxEv, authored) - maxEv);
    }
    if (maxEv < minEv)
        maxEv = minEv;
}

} // namespace GameEngine::Rendering
