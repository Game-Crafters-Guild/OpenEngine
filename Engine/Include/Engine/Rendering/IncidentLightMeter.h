#pragma once

#include <optional>
#include <span>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
struct ExtractedLight;

// The light falling on a horizontal surface: what Auto exposure's lights meter
// (Components::ExposureMeter::Lights) reads, as an incident light meter does. Never the pixels.
struct IncidentLight
{
    // What the directional lights the renderer shades deliver to a horizontal surface, in lux: the
    // sum, over SelectShadedDirectionals, of each light's delivered lux times max(0, the sine of its
    // elevation), so a light below the horizon or one that casts no light adds nothing.
    float DirectLux = 0.0f;
    // The image-based light on an up-facing surface, in lux, as ibl.glsl lights the scene with it:
    // the sky's, or a reflection probe's while one replaces the sky, times its IblIntensity.
    float SkyLux = 0.0f;
};

// The image-based half of the reading, as the renderer holds it now.
struct IncidentEnvironment
{
    // An environment source with content lights the scene: the sky, or a reflection probe that
    // replaces it.
    bool Lit = false;
    // Its newest irradiance reading on an up-facing surface, in lux; absent until the first lands.
    std::optional<float> Lux;
    // The source's IblIntensity, which ibl.glsl multiplies into what it samples.
    float IblIntensity = 1.0f;
};

// Reads the environment term from the IBL feature's active source and the irradiance readback,
// creating the readback on first use. Render thread only (IrradianceReadbackFeature's contract).
IncidentEnvironment ReadIncidentEnvironment(RenderServices& services);

// The incident light from a world's finalized light list (RenderServices::GetWorldLights) and the
// environment. Empty while an environment lights the scene and its first reading has not landed:
// the readback keeps its last reading between bakes, so this is empty only before the first one.
std::optional<IncidentLight> MeasureIncidentLight(std::span<const ExtractedLight> sortedLights,
                                                  const IncidentEnvironment& environment);

// The EV100 an incident meter reads for `light`: log2(E / 2.5) with E = DirectLux + SkyLux in lux
// (ISO 2720, C = 250; a 100 000 lx overhead sun reads 15.3). Unclamped: Auto's Min/Max EV clamp it.
// A scene with no daylight at all (E not positive), neither a directional light nor a sky, reads
// Components::kDefaultManualExposureEv, a sunlit exterior.
float IncidentLightEv100(const IncidentLight& light);

} // namespace GameEngine::Engine::Renderer
