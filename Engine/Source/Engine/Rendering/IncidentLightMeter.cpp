#include "Engine/Rendering/IncidentLightMeter.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IrradianceReadbackFeature.h"
#include "Engine/Rendering/RenderServices.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace GameEngine::Engine::Renderer
{

namespace
{
// The incident meter's calibration for a flat receiver: EV100 = log2(E x S / C) with S = 100 and
// C = 250 (ISO 2720), so E / 2.5 with E in lux.
constexpr float kIncidentMeterLuxPerEv0 = 2.5f;

// The EV100 a scene with no daylight at all reads: a sunlit exterior.
constexpr float kNoDaylightEv100 = Components::kDefaultManualExposureEv;

// What one shaded light delivers to a horizontal surface, in lux: its delivered lux (color
// luminance times intensity, as SkySunIlluminance::DeliveredLux resolves it at extraction) times the
// up component of the direction toward it (directionWS is the shine direction, so minus it). 0 for a
// light below the horizon.
float HorizontalLux(const ExtractedLight& light)
{
    const float elevationSine = -light.directionWS[1];
    const float deliveredLux =
        Components::UnitlessToLightIntensity(DeliveredLuminance(light), Components::LightUnit::Lux);
    return deliveredLux * std::max(0.0f, elevationSine);
}

// The sum of HorizontalLux over the directionals the renderer shades (SelectShadedDirectionals): the
// primary and the strongest emitting secondaries, up to kMaxDirectionalLights; the rest are not drawn.
float DirectionalLux(std::span<const ExtractedLight> sortedLights)
{
    std::array<const ExtractedLight*, kMaxDirectionalLights> shaded{};
    const uint32_t count = SelectShadedDirectionals(sortedLights, shaded);
    float lux = 0.0f;
    for (uint32_t i = 0; i < count; ++i)
        if (shaded[i]->castsLight != 0u)
            lux += HorizontalLux(*shaded[i]);
    return lux;
}
} // namespace

IncidentEnvironment ReadIncidentEnvironment(RenderServices& services)
{
    IncidentEnvironment out;
    const auto* ibl = services.GetFeature<ImageBasedLightingFeature>();
    const IEnvironmentSource* source = ibl ? ibl->GetEnvironmentSource() : nullptr;
    if (!source || !source->HasActiveContent())
        return out;
    out.Lit = true;
    out.IblIntensity = source->IblIntensity();
    float lux = 0.0f;
    if (services.EnsureFeature<IrradianceReadbackFeature>().TryGetUpIrradianceLux(lux))
        out.Lux = lux;
    return out;
}

std::optional<IncidentLight> MeasureIncidentLight(std::span<const ExtractedLight> sortedLights,
                                                  const IncidentEnvironment& environment)
{
    IncidentLight light{};
    light.DirectLux = DirectionalLux(sortedLights);
    // The image-based term counts only while an environment lights the scene. Until its first
    // reading lands there is no measurement, so the meter never flashes a value without the sky.
    if (environment.Lit)
    {
        if (!environment.Lux)
            return std::nullopt;
        light.SkyLux = *environment.Lux * std::max(0.0f, environment.IblIntensity);
    }
    return light;
}

float IncidentLightEv100(const IncidentLight& light)
{
    const float horizontalLux = std::max(0.0f, light.DirectLux) + std::max(0.0f, light.SkyLux);
    if (!(horizontalLux > 0.0f) || !std::isfinite(horizontalLux))
        return kNoDaylightEv100;
    return std::log2(horizontalLux / kIncidentMeterLuxPerEv0);
}

} // namespace GameEngine::Engine::Renderer
