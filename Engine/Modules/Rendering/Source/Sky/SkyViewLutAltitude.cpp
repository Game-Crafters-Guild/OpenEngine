#include "Rendering/Sky/SkyViewLutAltitude.h"

#include "Mathematics/Vector3.h"
#include "Rendering/Sky/SkyPlanet.h"

#include <cmath>

namespace GameEngine {
namespace Rendering {
namespace {

constexpr float kLinearBandMeters = 250.0f;
constexpr float kLogStartMeters = 1000.0f;
constexpr uint32_t kLinearBandCount = 4u; // kLogStartMeters / kLinearBandMeters
constexpr float kBandsPerOctave = 4.0f;

} // namespace

float AltitudeAbovePlanetMeters(const Mathematics::Vector3& position, const SkyPlanet& planet)
{
    const double x = static_cast<double>(position[0]) - planet.Center[0];
    const double y = static_cast<double>(position[1]) - planet.Center[1];
    const double z = static_cast<double>(position[2]) - planet.Center[2];
    return static_cast<float>(std::sqrt(x * x + y * y + z * z) - planet.Radius);
}

Mathematics::Vector3 SkyViewLutEyePosition(const Mathematics::Vector3& eye, const SkyPlanet& planet, uint32_t band)
{
    const float altitude = SkyViewLutBandAltitude(band) * planet.AtmosphereScale();
    if (!planet.Spherical)
        return Mathematics::Vector3(0.0f, altitude, 0.0f);

    const double x = static_cast<double>(eye[0]) - planet.Center[0];
    const double y = static_cast<double>(eye[1]) - planet.Center[1];
    const double z = static_cast<double>(eye[2]) - planet.Center[2];
    const double length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 0.0))
        return Mathematics::Vector3(planet.Center[0], planet.Center[1] + planet.Radius + altitude, planet.Center[2]);
    const double scale = (static_cast<double>(planet.Radius) + altitude) / length;
    return Mathematics::Vector3(static_cast<float>(planet.Center[0] + x * scale),
                                static_cast<float>(planet.Center[1] + y * scale),
                                static_cast<float>(planet.Center[2] + z * scale));
}

uint32_t SkyViewLutAltitudeBand(float altitudeMeters)
{
    if (!(altitudeMeters > 0.0f))
        return 0u;
    if (altitudeMeters < kLogStartMeters)
        return static_cast<uint32_t>(altitudeMeters / kLinearBandMeters);
    uint32_t band = kLinearBandCount +
                    static_cast<uint32_t>(std::floor(kBandsPerOctave * std::log2(altitudeMeters / kLogStartMeters)));
    // The logarithm rounds either way at a band's lower edge; the edge belongs to its band.
    if (SkyViewLutBandAltitude(band) > altitudeMeters)
        --band;
    else if (SkyViewLutBandAltitude(band + 1u) <= altitudeMeters)
        ++band;
    return band;
}

float SkyViewLutBandAltitude(uint32_t band)
{
    if (band < kLinearBandCount)
        return static_cast<float>(band) * kLinearBandMeters;
    return kLogStartMeters * std::exp2(static_cast<float>(band - kLinearBandCount) / kBandsPerOctave);
}

} // namespace Rendering
} // namespace GameEngine
