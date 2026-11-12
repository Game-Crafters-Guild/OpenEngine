#pragma once

#include "Mathematics/Vector3.h"

namespace GameEngine {
namespace Rendering {

/// The planet the sky wraps: the sphere the atmosphere sits on and the camera measures up from.
/// The default is the flat scene's planet: Earth's radius, center straight below the world origin.
/// A scene with a spherical terrain sets the terrain's center, radius and atmosphere height.
struct SkyPlanet
{
    static constexpr float kEarthRadiusMeters = 6360.0e3f;
    /// The height of Earth's atmosphere top above the ground: the flat scene's atmosphere.
    static constexpr float kEarthAtmosphereHeightMeters = 100.0e3f;
    /// Earth's atmosphere height as a share of its radius: a spherical terrain's default.
    static constexpr float kEarthAtmosphereShare = kEarthAtmosphereHeightMeters / kEarthRadiusMeters;

    /// The planet's center in world space, in meters.
    Mathematics::Vector3 Center{0.0f, -kEarthRadiusMeters, 0.0f};
    /// The planet's reference radius in meters: the ground of the atmosphere.
    float Radius = kEarthRadiusMeters;
    /// The height of the atmosphere's top above the ground, in meters. The air is Earth's air scaled
    /// to this height, so the zenith's optical depth, and the sky's color, does not change with it.
    float AtmosphereHeight = kEarthAtmosphereHeightMeters;
    /// True when a spherical terrain sets the planet. False for the flat scene's planet.
    bool Spherical = false;

    /// The atmosphere's height over Earth's: 1 on the flat scene's planet.
    float AtmosphereScale() const { return AtmosphereHeight / kEarthAtmosphereHeightMeters; }
};

} // namespace Rendering
} // namespace GameEngine
