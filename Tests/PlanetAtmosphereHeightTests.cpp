// TerrainECS::AtmosphereHeightMeters: the one atmosphere height the sky and the Inspector read, for
// each mode, on the shipped planets P-2 (radius 2 km, relief amplitude 50 m) and P-600 (600 km, 4 km).

#include <gtest/gtest.h>

#include "TerrainECS/SurfaceHeightQuery.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Rendering/Sky/SkyPlanet.h"

using namespace GameEngine;

namespace
{

// Everest's 8849 m on Earth's 100 km of atmosphere.
constexpr float kEverestShare = 0.08849f;

Components::Terrain Planet(float radius, Components::TerrainAtmosphereMode mode)
{
    Components::Terrain terrain{};
    terrain.Domain = Components::TerrainDomain::Spherical;
    terrain.PlanetRadius = radius;
    terrain.AtmosphereMode = mode;
    return terrain;
}

Components::TerrainPlanetRelief Relief(float amplitude)
{
    Components::TerrainPlanetRelief relief{};
    relief.Amplitude = amplitude;
    return relief;
}

} // namespace

// P-2's ridges reach 1.5 x 50 m = 75 m. Earth's share puts the air top at 31 m, below them; Auto puts
// the ridges at Everest's share of the air.
TEST(PlanetAtmosphereHeight, AutoOnASmallPlanetPutsTheHighestCrestAtEverestsShareOfTheAir)
{
    const Components::Terrain p2 = Planet(2000.0f, Components::TerrainAtmosphereMode::Auto);
    const Components::TerrainPlanetRelief relief = Relief(50.0f);
    const float height = TerrainECS::AtmosphereHeightMeters(p2, &relief);
    EXPECT_NEAR(height, 75.0f / kEverestShare, 0.5f);
    EXPECT_GT(height, 2000.0f * Rendering::SkyPlanet::kEarthAtmosphereShare);
}

// P-600's crests reach 6 km: Auto gives 67.8 km, above Earth's share (9.4 km).
TEST(PlanetAtmosphereHeight, AutoOnP600FollowsItsRelief)
{
    const Components::Terrain p600 = Planet(600.0e3f, Components::TerrainAtmosphereMode::Auto);
    const Components::TerrainPlanetRelief relief = Relief(4000.0f);
    EXPECT_NEAR(TerrainECS::AtmosphereHeightMeters(p600, &relief), 6000.0f / kEverestShare, 5.0f);
}

// Relief below Everest's share of Earth's share keeps Earth's share; no relief component counts as none.
TEST(PlanetAtmosphereHeight, AutoWithLowReliefKeepsEarthsShare)
{
    const Components::Terrain p600 = Planet(600.0e3f, Components::TerrainAtmosphereMode::Auto);
    const Components::TerrainPlanetRelief low = Relief(100.0f);
    const float earthLike = 600.0e3f * Rendering::SkyPlanet::kEarthAtmosphereShare;
    EXPECT_FLOAT_EQ(TerrainECS::AtmosphereHeightMeters(p600, &low), earthLike);
    EXPECT_FLOAT_EQ(TerrainECS::AtmosphereHeightMeters(p600, nullptr), earthLike);
}

// Auto never goes above the radius, the share field's own limit.
TEST(PlanetAtmosphereHeight, AutoIsAtMostTheRadius)
{
    const Components::Terrain tiny = Planet(100.0f, Components::TerrainAtmosphereMode::Auto);
    const Components::TerrainPlanetRelief relief = Relief(50.0f);
    EXPECT_FLOAT_EQ(TerrainECS::AtmosphereHeightMeters(tiny, &relief), 100.0f);
}

// The authored modes ignore the relief and read their own value.
TEST(PlanetAtmosphereHeight, TheAuthoredModesReadTheirOwnValue)
{
    const Components::TerrainPlanetRelief relief = Relief(50.0f);
    Components::Terrain share = Planet(2000.0f, Components::TerrainAtmosphereMode::ShareOfRadius);
    share.AtmosphereShare = Rendering::SkyPlanet::kEarthAtmosphereShare;
    share.AtmosphereHeight = 60.0f;
    EXPECT_NEAR(TerrainECS::AtmosphereHeightMeters(share, &relief), 31.45f, 0.01f);

    Components::Terrain meters = share;
    meters.AtmosphereMode = Components::TerrainAtmosphereMode::HeightMeters;
    EXPECT_FLOAT_EQ(TerrainECS::AtmosphereHeightMeters(meters, &relief), 60.0f);
}

// Height in Meters stops at the radius, as Auto and Share of Radius do.
TEST(PlanetAtmosphereHeight, HeightInMetersIsAtMostTheRadius)
{
    Components::Terrain meters = Planet(2000.0f, Components::TerrainAtmosphereMode::HeightMeters);
    meters.AtmosphereHeight = 1.0e9f;
    EXPECT_FLOAT_EQ(TerrainECS::AtmosphereHeightMeters(meters, nullptr), 2000.0f);
}

// A script can write a radius at or below 0: every mode then gives no air.
TEST(PlanetAtmosphereHeight, ARadiusAtOrBelowZeroGivesZeroInEveryMode)
{
    const Components::TerrainPlanetRelief relief = Relief(50.0f);
    for (const float radius : {0.0f, -2000.0f})
        for (const auto mode : {Components::TerrainAtmosphereMode::Auto, Components::TerrainAtmosphereMode::ShareOfRadius,
                                Components::TerrainAtmosphereMode::HeightMeters})
            EXPECT_EQ(TerrainECS::AtmosphereHeightMeters(Planet(radius, mode), &relief), 0.0f)
                << radius << " " << static_cast<int>(mode);
}
