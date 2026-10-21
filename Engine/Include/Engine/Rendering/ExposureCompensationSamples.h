#pragma once

#include "Types/Types.h"

namespace GameEngine::Engine::Renderer
{

// The exposure compensation curve (Components::ExposureCompensationCurve.h) as the auto exposure
// resolve reads it: kCount evenly spaced samples, in stops, from the first key's EV100 to the last;
// auto_exposure_resolve.comp interpolates them linearly and holds the end samples outside. Built once
// per view per frame by SampleExposureCompensation (Engine/Rendering/Exposure.h). The default value
// (all zero) adds nothing at any EV.
struct ExposureCompensationSamples
{
    // The resolve's push constants hold the samples as four vec4: 16 floats, inside the 128-byte
    // push-constant budget every backend guarantees.
    static constexpr uint32 kCount = 16;

    float32 StartEv = 0.0f; // EV100 of Stops[0]
    float32 EndEv = 0.0f;   // EV100 of Stops[kCount - 1]; equal to StartEv for a constant curve
    float32 Stops[kCount] = {};
};

} // namespace GameEngine::Engine::Renderer
