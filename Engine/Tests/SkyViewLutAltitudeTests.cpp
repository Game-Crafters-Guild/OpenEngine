// The sky-view LUT's altitude bands (Rendering/Sky/SkyViewLutAltitude.h): the LUT rebakes only when
// the camera crosses from one band into another, and band 0 bakes at the ground. The transmittance
// LUT's key: the planet's radius and atmosphere height.

#include <gtest/gtest.h>

#include "Rendering/Sky/SkyViewLutAltitude.h"

#include "Mathematics/Vector3.h"
#include "Rendering/Sky/SkyPlanet.h"
#include "Rendering/Sky/SkyRenderer.h"

#include <cmath>
#include <cstdint>

using GameEngine::Mathematics::Vector3;
using GameEngine::Rendering::AltitudeAbovePlanetMeters;
using GameEngine::Rendering::SkyPlanet;
using GameEngine::Rendering::SkyRenderer;
using GameEngine::Rendering::SkyViewLutAltitudeBand;
using GameEngine::Rendering::SkyViewLutBandAltitude;
using GameEngine::Rendering::SkyViewLutEyePosition;

namespace {

constexpr float kPlanetRadius = 600.0e3f; // the P-600 probe scene

SkyPlanet SphericalPlanet()
{
    SkyPlanet planet;
    planet.Center = Vector3(0.0f, 0.0f, 0.0f);
    planet.Radius = kPlanetRadius;
    planet.Spherical = true;
    return planet;
}

Vector3 Direction(const Vector3& v)
{
    const double length = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y +
                                    static_cast<double>(v.z) * v.z);
    return Vector3(static_cast<float>(v.x / length), static_cast<float>(v.y / length), static_cast<float>(v.z / length));
}

} // namespace

// The sky's up at four eyes on a planet: the LUT's eye is above the ground under the eye, at the
// band's altitude, so its up is the eye's local up and never world +Y.
TEST(SkyViewLutAltitude, OnAPlanetTheLutEyeIsAboveTheGroundUnderTheEye)
{
    const SkyPlanet planet = SphericalPlanet();
    const Vector3 corner = Direction(Vector3(1.0f, 1.0f, 1.0f));
    struct Eye
    {
        const char* Name;
        Vector3 Up;
        float Altitude;
    };
    const Eye eyes[] = {
        {"pole", Vector3(0.0f, 1.0f, 0.0f), 1.7f},
        {"equator", Vector3(1.0f, 0.0f, 0.0f), 1.7f},
        {"cube corner", corner, 1.7f},
        {"100 km above the pole", Vector3(0.0f, 1.0f, 0.0f), 100.0e3f},
    };
    for (const Eye& eye : eyes)
    {
        const Vector3 position = eye.Up * (kPlanetRadius + eye.Altitude);
        EXPECT_NEAR(AltitudeAbovePlanetMeters(position, planet), eye.Altitude, 0.05f) << eye.Name;

        const uint32_t band = SkyViewLutAltitudeBand(AltitudeAbovePlanetMeters(position, planet));
        const Vector3 lutEye = SkyViewLutEyePosition(position, planet, band);
        const Vector3 lutUp = Direction(lutEye);
        EXPECT_NEAR(lutUp.x, eye.Up.x, 1e-6f) << eye.Name;
        EXPECT_NEAR(lutUp.y, eye.Up.y, 1e-6f) << eye.Name;
        EXPECT_NEAR(lutUp.z, eye.Up.z, 1e-6f) << eye.Name;
        EXPECT_NEAR(AltitudeAbovePlanetMeters(lutEye, planet), SkyViewLutBandAltitude(band), 0.1f) << eye.Name;
    }
}

// A 2 km planet with Earth's share of air (31.45 m): the band altitudes are Earth's, scaled by the
// atmosphere's height over Earth's, so each band bakes at the same share of the air as on Earth.
TEST(SkyViewLutAltitude, OnASmallPlanetTheLutEyeAltitudeScalesWithTheAtmosphere)
{
    SkyPlanet planet = SphericalPlanet();
    planet.Radius = 2000.0f;
    planet.AtmosphereHeight = 31.45f;
    const Vector3 up(0.0f, 1.0f, 0.0f);
    const Vector3 eye = up * (planet.Radius + 1.7f);
    for (const uint32_t band : {4u, 12u, 30u})
    {
        const Vector3 lutEye = SkyViewLutEyePosition(eye, planet, band);
        EXPECT_NEAR(AltitudeAbovePlanetMeters(lutEye, planet),
                    SkyViewLutBandAltitude(band) * planet.AtmosphereScale(), 1e-3f)
            << "band " << band;
    }
    // Band 4 bakes 1 km up on Earth, so 1 km times 31.45 / 100000 here.
    EXPECT_NEAR(AltitudeAbovePlanetMeters(SkyViewLutEyePosition(eye, planet, 4u), planet), 0.3145f, 1e-3f);
}

// The transmittance LUT depends on the atmosphere height as well as the radius: a new height at the
// same radius makes the baked LUT stale.
TEST(SkyViewLutAltitude, TheTransmittanceLutIsStaleAfterTheAtmosphereHeightChanges)
{
    SkyRenderer sky;
    SkyPlanet planet = SphericalPlanet();
    sky.MarkTransmittanceComputed(planet);
    EXPECT_TRUE(sky.IsTransmittanceCurrentFor(planet));

    planet.AtmosphereHeight = 0.5f * SkyPlanet::kEarthAtmosphereHeightMeters;
    EXPECT_FALSE(sky.IsTransmittanceCurrentFor(planet));

    sky.MarkTransmittanceComputed(planet);
    EXPECT_TRUE(sky.IsTransmittanceCurrentFor(planet));
}

// A flat scene's sky is unchanged: the LUT bakes at (0, band altitude, 0) wherever the eye is, and
// the altitude is the one the sky measured with its center fixed below the world origin.
TEST(SkyViewLutAltitude, OnTheFlatPlanetTheLutEyeAndTheAltitudeAreUnchanged)
{
    const SkyPlanet flat;
    EXPECT_FALSE(flat.Spherical);
    EXPECT_EQ(flat.Center.y, -SkyPlanet::kEarthRadiusMeters);
    EXPECT_EQ(flat.Radius, 6360.0e3f);

    const Vector3 eyes[] = {Vector3(0.0f, 1.7f, 0.0f), Vector3(12000.0f, 350.0f, -8000.0f),
                            Vector3(-250.0e3f, 20.0e3f, 90.0e3f)};
    for (const Vector3& eye : eyes)
    {
        const double y = static_cast<double>(eye.y) + flat.Radius;
        const double r = std::sqrt(static_cast<double>(eye.x) * eye.x + y * y + static_cast<double>(eye.z) * eye.z);
        const float altitude = AltitudeAbovePlanetMeters(eye, flat);
        EXPECT_EQ(altitude, static_cast<float>(r - flat.Radius));

        const uint32_t band = SkyViewLutAltitudeBand(altitude);
        const Vector3 lutEye = SkyViewLutEyePosition(eye, flat, band);
        EXPECT_EQ(lutEye.x, 0.0f);
        EXPECT_EQ(lutEye.y, SkyViewLutBandAltitude(band));
        EXPECT_EQ(lutEye.z, 0.0f);
    }
}

TEST(SkyViewLutAltitude, ACameraMovingInsideABandKeepsItsBand)
{
    EXPECT_EQ(SkyViewLutAltitudeBand(-50.0f), 0u);
    EXPECT_EQ(SkyViewLutAltitudeBand(0.0f), 0u);
    EXPECT_EQ(SkyViewLutAltitudeBand(249.0f), 0u);
    EXPECT_EQ(SkyViewLutAltitudeBand(250.0f), 1u);
    EXPECT_EQ(SkyViewLutAltitudeBand(999.0f), 3u);
    EXPECT_EQ(SkyViewLutAltitudeBand(1000.0f), 4u);
    EXPECT_EQ(SkyViewLutAltitudeBand(1180.0f), 4u);
    EXPECT_EQ(SkyViewLutAltitudeBand(1190.0f), 5u);
    EXPECT_FLOAT_EQ(SkyViewLutBandAltitude(0u), 0.0f);
}

TEST(SkyViewLutAltitude, AClimbToTheTopOfTheAtmosphereCrossesThirtyBands)
{
    EXPECT_EQ(SkyViewLutAltitudeBand(100000.0f), 30u);
    for (uint32_t band = 0; band <= 30u; ++band)
    {
        EXPECT_EQ(SkyViewLutAltitudeBand(SkyViewLutBandAltitude(band)), band) << "lower edge of band " << band;
        EXPECT_LT(SkyViewLutBandAltitude(band), SkyViewLutBandAltitude(band + 1u)) << "band " << band;
    }
}
