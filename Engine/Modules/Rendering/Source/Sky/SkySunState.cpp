#include "Rendering/Sky/SkySunState.h"

#include "Rendering/Sky/SkySystem.h"
#include "Rendering/Sky/SolarPath.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Rendering {
namespace {

constexpr float kTwoPi = 6.2831853f;

Mathematics::Radians ElevationOf(const float direction[3])
{
    return std::asin(std::clamp(direction[1], -1.0f, 1.0f));
}

// How far the horizon of a sphere of `planetRadiusMeters` lies below the horizontal for an eye
// `altitudeMeters` above its surface: acos(R / (R + h)). 0 at or below the surface; about 3.2
// degrees at 10 km on the sky's planet.
Mathematics::Radians HorizonDipRadians(float altitudeMeters, float planetRadiusMeters)
{
    if (!(altitudeMeters > 0.0f) || !(planetRadiusMeters > 0.0f))
        return 0.0f;
    // In double: R / (R + h) sits within 1e-6 of 1 for a camera a few meters up, where a float
    // acos is quantized to a few hundredths of a degree.
    const double radius = planetRadiusMeters;
    return static_cast<float>(std::acos(radius / (radius + altitudeMeters)));
}

} // namespace

SkySunState ComputeSkySunState(const SolarFrame& frame,
                               const float sunDir[3],
                               const float moonDir[3],
                               float moonArcPosition,
                               float cameraAltitudeMeters,
                               float planetRadiusMeters)
{
    SkySunState state;
    state.SunElevation = ElevationOf(sunDir);
    float azimuth = SolarBearingRadians(frame, sunDir);
    if (azimuth < 0.0f)
        azimuth += kTwoPi;
    state.SunAzimuth = azimuth < kTwoPi ? azimuth : 0.0f;
    state.SunApparentElevation =
        state.SunElevation + HorizonDipRadians(cameraAltitudeMeters, planetRadiusMeters);
    state.MoonElevation = ElevationOf(moonDir);
    state.MoonPhaseFraction = MoonPhaseIlluminanceFraction(moonArcPosition);
    const float cosSeparation = sunDir[0] * moonDir[0] + sunDir[1] * moonDir[1] + sunDir[2] * moonDir[2];
    state.MoonSunSeparation = std::acos(std::clamp(cosSeparation, -1.0f, 1.0f));
    return state;
}

} // namespace Rendering
} // namespace GameEngine
