#pragma once

// The per-view exposure stamp RenderExtractionSystem applies after the volume blend. Private to the
// engine's extraction; ExposureTests includes it by path.

#include "Components/Rendering/Camera.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Mathematics/Interpolation.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{

// Overwrite a view's exposure with its camera's: the camera owns the sensor; volumes contribute only
// grading, plus the Exposure Adjustment modifiers stacked on top. Called by the per-view post-process
// resolve (RenderExtractionSystem) for a camera-driven view, and with a default CameraExposure for a
// view with no camera.
inline void ApplyCameraExposure(PostProcessSettings& s, const ViewRegistry::CameraExposure& ce)
{
    const auto mode = static_cast<Components::ExposureMode>(ce.Mode);
    const float ev100 = Rendering::ExposureEv100ForMode(mode, ce.ManualExposureEV,
                                                        ce.Aperture, ce.ShutterTime, ce.Iso);
    s.Exposure = Rendering::ResolveExposureScale(mode, ce.Exposure, ev100, ce.ExposureCompensation);
    if (mode == Components::ExposureMode::Auto)
    {
        s.AutoExposureActive    = true;
        s.AutoExposureMinEv     = ce.AutoExposureMinEv;
        s.AutoExposureMaxEv     = ce.AutoExposureMaxEv;
        s.AutoExposureSpeedUp   = ce.AutoExposureSpeedUp;
        s.AutoExposureSpeedDown = ce.AutoExposureSpeedDown;
        s.AutoExposureMeter     = ce.AutoExposureMeter;
        s.AutoExposureCompensation = Rendering::SampleExposureCompensation(ce.AutoExposureCompensationCurve);
        // All EV trims ride the bias, applied post-clamp (TryWriteField), so compensation
        // stays effective even when adaptation is pinned at the MinEv/MaxEv envelope edge. The
        // compensation curve applies post-clamp too, at the meter's reading.
        // The default bias is the engine's display-brightness anchor for Auto metering
        // (see kDefaultAutoExposureBiasEv) — Manual/Physical/Fixed take no default trim.
        s.AutoExposureBiasEv    = kDefaultAutoExposureBiasEv + ce.ExposureCompensation;
    }
    else
    {
        s.AutoExposureActive = false;
    }
    // The lens: physical DoF reads the camera's focus plane, derived focal
    // length, and the same f-number that drives physical exposure.
    s.DofFocusDistance = std::max(ce.FocusDistance, Components::Camera::kFocusDistanceMin);
    s.DofFocalLengthMm = std::max(ce.FocalLengthMm, 1.0f);
    s.DofAperture = std::max(ce.Aperture, Components::Camera::kApertureMin);
    s.DofSensorHeightMm = std::max(ce.SensorHeightMm, 1.0f);
    s.DofApertureBladeCount = std::clamp(
        ce.ApertureBladeCount,
        static_cast<int32>(Components::Camera::kApertureBladeCountMin),
        static_cast<int32>(Components::Camera::kApertureBladeCountMax));
    s.DofApertureRoundness = Math::Clamp01(ce.ApertureRoundness);
    s.DofApertureRotation = std::clamp(ce.ApertureRotation, 0.0f, 360.0f);
    s.DofAnamorphicSqueeze = std::clamp(ce.AnamorphicSqueeze, 1.0f, 4.0f);
    s.DofDebugMode = ce.FocusDebugMode != 0 ? 1 : 0;
    s.DofDebugAlpha = Math::Clamp01(ce.FocusDebugAlpha);
    // Volume exposure modifiers (ExposureAdjustmentEffect) ride the blended settings and
    // stack on the sensor resolve above: compensation in log space (added to the post-clamp
    // metering bias in Auto, like the camera's own compensation); the clamps only narrow
    // the adaptation envelope — a volume can tighten the camera's range, never widen it.
    if (s.AutoExposureActive)
    {
        Rendering::NarrowAutoExposureEnvelope(s.AutoExposureMinEv, s.AutoExposureMaxEv, s.ExposureClampMinEv,
                                              s.ExposureClampMinWeight, s.ExposureClampMaxEv,
                                              s.ExposureClampMaxWeight);
        s.AutoExposureBiasEv += s.ExposureCompensationEv;
    }
    else
    {
        s.Exposure *= std::exp2(s.ExposureCompensationEv);
    }
}

} // namespace GameEngine::Engine::Renderer
