#pragma once

#include "Mathematics/Angle.h"

namespace GameEngine {
namespace Rendering {

struct SolarFrame;

/// Where the sun and the moon stand this frame, as the sky places them. The sky system writes it
/// once per frame beside the sky settings; the volume extraction carries it to the effects. Angles
/// are radians. Elevations are measured against world +Y and are negative below the horizon. The
/// moon fields give the moon's place whether or not the sky draws it; with a manual sun direction
/// the moon stands opposite the sun. The defaults are the sky with no sky environment: the sun
/// overhead and a full moon underfoot.
struct SkySunState
{
    /// The sun's elevation above the geometric horizon, in [-pi / 2, pi / 2].
    Mathematics::Radians SunElevation = 1.5707963f;
    /// The sun's compass bearing on the site, in [0, 2 pi): 0 = north, pi / 2 = east.
    Mathematics::Radians SunAzimuth = 0.0f;
    /// The sun's elevation above the apparent horizon at the main camera's altitude: SunElevation
    /// plus the horizon's dip at that altitude, acos(R / (R + h)) for the sky's planet radius R.
    /// Equal to SunElevation at or below the planet surface.
    Mathematics::Radians SunApparentElevation = 1.5707963f;
    /// The moon's elevation above the geometric horizon, in [-pi / 2, pi / 2].
    Mathematics::Radians MoonElevation = -1.5707963f;
    /// The moon's illuminance as a fraction of a full moon's, from the phase law
    /// (MoonPhaseIlluminanceFraction): 1 at full moon, 0 at new moon.
    float MoonPhaseFraction = 1.0f;
    /// The angle between the moon and the sun on the sky, in [0, pi].
    Mathematics::Radians MoonSunSeparation = 3.1415927f;
};

/// The sun state for a sun and a moon toward the unit directions `sunDir` and `moonDir`, the moon at
/// `moonArcPosition` on its cycle (0 new, 0.5 full), on the site `frame` describes, seen by a camera
/// `cameraAltitudeMeters` above a planet of `planetRadiusMeters`.
SkySunState ComputeSkySunState(const SolarFrame& frame,
                               const float sunDir[3],
                               const float moonDir[3],
                               float moonArcPosition,
                               float cameraAltitudeMeters,
                               float planetRadiusMeters);

} // namespace Rendering
} // namespace GameEngine
