#pragma once

#include <cstdint>

namespace GameEngine {
namespace Mathematics {
struct Vector3;
}
namespace Rendering {
struct SkyPlanet;

// The sky-view LUT is baked at the lower edge of the camera's altitude band. Bands are 250 m wide
// to 1 km, then a quarter octave each, in Earth's atmosphere: a planet's altitude is divided by
// SkyPlanet::AtmosphereScale first. At or below the surface the band is 0.

/// The height above the surface of `planet`, in meters, of the point at world `position`. Negative
/// below the surface.
float AltitudeAbovePlanetMeters(const Mathematics::Vector3& position, const SkyPlanet& planet);

/// The world position the sky-view LUT is baked from for an eye at `eye` in `band`: on the flat
/// planet (0, band altitude, 0), so the LUT's up is +Y; on a spherical planet the point at the band
/// altitude, times the atmosphere scale, above the surface under the eye.
Mathematics::Vector3 SkyViewLutEyePosition(const Mathematics::Vector3& eye, const SkyPlanet& planet, uint32_t band);

/// The band a camera at `altitudeMeters` above the planet surface is in.
uint32_t SkyViewLutAltitudeBand(float altitudeMeters);

/// The altitude the sky-view LUT is baked at for `band`: the band's lower edge, so band 0 bakes at
/// the ground.
float SkyViewLutBandAltitude(uint32_t band);

} // namespace Rendering
} // namespace GameEngine
