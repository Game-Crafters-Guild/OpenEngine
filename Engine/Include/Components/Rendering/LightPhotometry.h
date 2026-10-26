#pragma once

#include "Components/Rendering/Light.h" // LightType, LightUnit
#include "Types/Color.h"

#include <algorithm>
#include <cmath>

// CPU-side photometric helpers for lights. The extraction system resolves a light's authored
// color temperature + physical-intensity unit into the engine's plain linear color / unitless
// intensity here, so the GPU path is unchanged (zero shader cost).
namespace GameEngine::Components
{

/// Returns the linear tint of a correlated color temperature in kelvin. The brightest channel is 1,
/// so the tint changes the hue and not the brightness. The fit is the Tanner-Helland blackbody
/// approximation, valid from 1000 K to 40000 K; it gives sRGB values, which are then decoded to
/// linear. 6500 K is close to white.
inline ColorLinear KelvinToLinearRGB(float kelvin)
{
    const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;

    constexpr float kByteMax = 255.0f;
    float r, g, b;
    if (t <= 66.0f)
    {
        r = kByteMax;
        g = std::clamp(99.4708025861f * std::log(t) - 161.1195681661f, 0.0f, kByteMax);
    }
    else
    {
        r = std::clamp(329.698727446f * std::pow(t - 60.0f, -0.1332047592f), 0.0f, kByteMax);
        g = std::clamp(288.1221695283f * std::pow(t - 60.0f, -0.0755148492f), 0.0f, kByteMax);
    }
    if (t >= 66.0f)
        b = kByteMax;
    else if (t <= 19.0f)
        b = 0.0f;
    else
        b = std::clamp(138.5177312231f * std::log(t - 10.0f) - 305.0447927307f, 0.0f, kByteMax);

    // Every branch of the fit sets one channel to 255, so the brightest linear channel is exactly 1.
    return ColorSRGB(r / kByteMax, g / kByteMax, b / kByteMax).ToLinear();
}

// Physical-intensity unit calibration anchor, shared by the forward + inverse conversions so they
// stay a matched pair. Lux (illuminance) and candela (luminous intensity) BOTH re-anchor to the
// single BT.2408 reference white of 203 nits — the same anchor as GE_EMISSION_PAPERWHITE_NITS in
// surface_io.glsl. This couples the INPUT SCALE of lights to emission, not the output radiance: a
// 203-nit emitter outputs scene-linear 1.0 directly (no BRDF), whereas a light of 203 lx (or 203 cd
// at 1 m) becomes unitless intensity 1.0, which the diffuse BRDF then carries through its 1/pi term,
// albedo, and NdotL — so a white normal-facing Lambertian under it reads ~1/pi, by design. The point
// is that "203 of either" is the same reference-white input, so authoring lights and emission on one
// scale is coherent (final look is set by exposure). kLightFourPi is the sphere solid angle
// (flux -> intensity geometry), not a calibration knob.
// BT.2408 reference white in nits — the single anchor both units re-scale to. Kept equal to the
// shader's GE_EMISSION_PAPERWHITE_NITS (surface_io.glsl); C++ can't include GLSL, so they are
// hand-synced by value (a tuning anchor, not an ABI constant).
inline constexpr float kReferenceWhiteNits = 203.0f;
inline constexpr float kUnitlessPerLux     = 1.0f / kReferenceWhiteNits;
inline constexpr float kUnitlessPerCandela = 1.0f / kReferenceWhiteNits;
inline constexpr float kLightFourPi        = 12.56637061f;

// Illuminance of the sun on a surface facing it at noon on a clear day, in lux: the default of every
// new directional light and of the sun the sky reads.
inline constexpr float kClearNoonSunIlluminanceLux = 100000.0f;

// Physical-intensity unit -> the engine's unitless intensity. Unitless passes through unchanged,
// so existing lights are unaffected.
inline float LightIntensityToUnitless(float intensity, LightUnit unit)
{
    switch (unit)
    {
    case LightUnit::Lux:     return intensity * kUnitlessPerLux;                       // illuminance (directional)
    case LightUnit::Candela: return intensity * kUnitlessPerCandela;                  // luminous intensity
    case LightUnit::Lumen:   return (intensity / kLightFourPi) * kUnitlessPerCandela; // flux -> candela (point/spot)
    case LightUnit::Unitless:
    default:                 return intensity;                                        // pass through
    }
}

// Inverse of LightIntensityToUnitless: a unitless intensity -> the value it would take in `unit`.
// Used to convert a light's Intensity when its unit changes so effective brightness is preserved.
inline float UnitlessToLightIntensity(float unitless, LightUnit unit)
{
    switch (unit)
    {
    case LightUnit::Lux:     return unitless / kUnitlessPerLux;
    case LightUnit::Candela: return unitless / kUnitlessPerCandela;
    case LightUnit::Lumen:   return (unitless / kUnitlessPerCandela) * kLightFourPi;
    case LightUnit::Unitless:
    default:                 return unitless;
    }
}

// Resolve a light's authored color/intensity into the plain values the GPU paths consume: Color
// tinted by color temperature (when enabled) + Intensity converted from its unit. Shared by every
// light consumer (world meshes, ocean, terrain) so they all render the same resolved light.
inline void ResolveLightColorIntensity(const Light& light, float outColor[3], float& outIntensity)
{
    const ColorLinear tint =
        light.UseColorTemperature ? KelvinToLinearRGB(light.ColorTemperature) : ColorLinear(1.0f, 1.0f, 1.0f);
    outColor[0] = light.Color[0] * tint.r;
    outColor[1] = light.Color[1] * tint.g;
    outColor[2] = light.Color[2] * tint.b;
    outIntensity = LightIntensityToUnitless(light.Intensity, light.IntensityUnit);
}

} // namespace GameEngine::Components
