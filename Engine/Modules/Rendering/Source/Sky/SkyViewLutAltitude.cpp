#include "Rendering/Sky/SkyViewLutAltitude.h"

#include <cmath>

namespace GameEngine {
namespace Rendering {
namespace {

constexpr float kLinearBandMeters = 250.0f;
constexpr float kLogStartMeters = 1000.0f;
constexpr uint32_t kLinearBandCount = 4u; // kLogStartMeters / kLinearBandMeters
constexpr float kBandsPerOctave = 4.0f;

} // namespace

float AltitudeAbovePlanetMeters(const float position[3], float planetRadius)
{
    const double y = static_cast<double>(position[1]) + planetRadius;
    const double r = std::sqrt(static_cast<double>(position[0]) * position[0] + y * y +
                               static_cast<double>(position[2]) * position[2]);
    return static_cast<float>(r - planetRadius);
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
