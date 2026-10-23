#pragma once

#include <cstdint>

namespace GameEngine {
namespace Rendering {

/// The camera altitudes the sky-view LUT is baked at. The dome a camera sees depends on its height
/// above the ground (the horizon dips below the horizontal and the air above thins), so the LUT is
/// baked for the altitude band the camera is in and rebaked only when the camera crosses into
/// another band: a camera that stays inside one band never rebakes.
///
/// Bands are 250 m wide from the ground to 1 km, then a quarter of an octave each (1 km, 1.19 km,
/// 1.41 km, ...): 31 bands from the ground to the top of a 100 km atmosphere, with the horizon's dip
/// moving about one LUT row from one band to the next. Altitudes are metres above the planet
/// surface; at or below the surface the band is 0.

/// The height above the planet surface, in meters, of the point at world `position`: the planet's
/// center sits `planetRadius` below the world origin, as in the sky shaders. Negative below the
/// surface.
float AltitudeAbovePlanetMeters(const float position[3], float planetRadius);

/// The band a camera at `altitudeMeters` above the planet surface is in.
uint32_t SkyViewLutAltitudeBand(float altitudeMeters);

/// The altitude the sky-view LUT is baked at for `band`: the band's lower edge, so band 0 bakes at
/// the ground.
float SkyViewLutBandAltitude(uint32_t band);

} // namespace Rendering
} // namespace GameEngine
