#pragma once

namespace GameEngine {
namespace Rendering {

// The constants of the simple sun and moon model the sky system runs (SkySystem.h), on its own so a
// component that derives a default from them does not take in the model's functions.
struct SkySystemConfig
{
    // If true, when the sun is sufficiently below the horizon we switch the primary light to the moon.
    bool useMoonWhenSunBelowHorizon = true;

    // Legacy midpoint for sun/moon handoff; the actual lighting cross-fade spans a band
    // around this value so primary color does not pop at sunset.
    float sunBelowHorizonDotThreshold = -0.05f;

    // A full moon's illuminance as a fraction of a clear sun's, both on a surface facing them high in
    // the sky: 0.25 lx against 100 000 lx (Krisciunas and Schaefer 1991, PASP 103:1033: V = -12.73,
    // about 0.27 lx above the atmosphere). The same fraction sets the moon's light, its in-scatter in
    // the sky and its disc; the phase scales all three (MoonPhaseIlluminanceFraction).
    float moonLightIlluminanceScale = 2.5e-6f;
    // The moon's colour at unit luminance, linear Rec.709: sunlight reflected by a surface redder than
    // grey (lunar B-V 0.92 against the sun's 0.65, Allen's Astrophysical Quantities), a reflectance
    // rising as lambda^1.1, sampled at the 680, 550 and 440 nm the sky's coefficients use. The
    // atmosphere reddens it further at moonrise through the same transmittance as the sun.
    // Includes/moon_disc.glsl carries the same values as GE_MOON_TINT for the sky-view LUT and the disc.
    float moonColor[3] = { 1.2142f, 0.9614f, 0.7521f };
};

} // namespace Rendering
} // namespace GameEngine
