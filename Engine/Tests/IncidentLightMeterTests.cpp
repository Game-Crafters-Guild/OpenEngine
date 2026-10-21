// Auto exposure's lights meter (IncidentLightMeter.h): the light falling on a horizontal surface from
// the directional lights the renderer shades and the sky, read as an incident meter reads it.

#include <gtest/gtest.h>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/IncidentLightMeter.h"
#include "Engine/Rendering/RenderServices.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

using namespace GameEngine;
using Engine::Renderer::IncidentEnvironment;
using Engine::Renderer::IncidentLight;
using Engine::Renderer::IncidentLightEv100;
using Engine::Renderer::MeasureIncidentLight;

namespace
{
constexpr float kOverheadSunLux = 100000.0f;

float DegreesSine(float degrees)
{
    return std::sin(degrees * std::numbers::pi_v<float> / 180.0f);
}

// The directional lights a world submits, as the renderer finalizes them: each Finalize submits
// them to a fresh RenderServices and returns its strongest-first list, the span the meter reads.
class FinalizedLights
{
  public:
    // A white directional light of `lux` shining down at `elevationDegrees` below the horizon toward
    // +Z (directionWS is the shine direction, as extraction leaves it). Returns its index for At.
    size_t Add(float lux, float elevationDegrees)
    {
        Engine::Renderer::ExtractedLight light{};
        light.type = Components::LightType::Directional;
        light.intensity = Components::LightIntensityToUnitless(lux, Components::LightUnit::Lux);
        light.SortId = static_cast<uint32_t>(m_Submitted.size() + 1);
        Aim(light, elevationDegrees);
        m_Submitted.push_back(light);
        return m_Submitted.size() - 1;
    }
    Engine::Renderer::ExtractedLight& At(size_t index) { return m_Submitted[index]; }
    static void Aim(Engine::Renderer::ExtractedLight& light, float elevationDegrees)
    {
        const float radians = elevationDegrees * std::numbers::pi_v<float> / 180.0f;
        light.directionWS[0] = 0.0f;
        light.directionWS[1] = -std::sin(radians);
        light.directionWS[2] = std::cos(radians);
    }
    std::span<const Engine::Renderer::ExtractedLight> Finalize()
    {
        m_Services = std::make_unique<Engine::Renderer::RenderServices>();
        for (const Engine::Renderer::ExtractedLight& light : m_Submitted)
            m_Services->SubmitLight(0u, light);
        m_Services->FinalizeWorldLights(0u);
        return m_Services->GetWorldLights(0u);
    }

  private:
    std::vector<Engine::Renderer::ExtractedLight> m_Submitted;
    std::unique_ptr<Engine::Renderer::RenderServices> m_Services;
};

// The EV the meter reads from `lights` under `environment`; fails the test when there is no reading.
float ReadEv(FinalizedLights& lights, const IncidentEnvironment& environment = {})
{
    const std::optional<IncidentLight> light = MeasureIncidentLight(lights.Finalize(), environment);
    EXPECT_TRUE(light.has_value());
    return light ? IncidentLightEv100(*light) : 0.0f;
}
} // namespace

// An overhead 100 000 lx sun reads EV100 15.3, the incident reading for a clear noon.
TEST(IncidentLightMeter, AnOverheadSunReadsItsFullIlluminance)
{
    EXPECT_NEAR(IncidentLightEv100({.DirectLux = kOverheadSunLux}), std::log2(kOverheadSunLux / 2.5f), 1e-4f);
}

// 10:00 at 40 degrees north on the equinox: the sun stands about 42 degrees up and a horizontal
// surface receives its lux times the sine of that elevation.
TEST(IncidentLightMeter, AMorningSunReadsTheSineOfItsElevation)
{
    Components::SkyEnvironment sky{};
    sky.TimeOfDayHours = 10.0f;
    sky.Latitude = 40.0f;
    const Rendering::SolarPosition sun = Rendering::SolarPositionAtHour(
        Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(sky)), sky.TimeOfDayHours);
    ASSERT_GT(sun.ElevationDegrees, 30.0f);
    ASSERT_LT(sun.ElevationDegrees, 50.0f);

    FinalizedLights lights;
    lights.Add(kOverheadSunLux, sun.ElevationDegrees);
    EXPECT_NEAR(ReadEv(lights), std::log2(kOverheadSunLux * DegreesSine(sun.ElevationDegrees) / 2.5f), 1e-3f);
}

// The sky adds to the direct light; with no direct light the sky alone sets the reading.
TEST(IncidentLightMeter, TheSkyAddsToTheSunAndStandsAloneWithoutIt)
{
    constexpr float kSkyLux = 15800.0f;
    const IncidentLight withSun{.DirectLux = kOverheadSunLux * DegreesSine(20.0f), .SkyLux = kSkyLux};
    EXPECT_NEAR(IncidentLightEv100(withSun), std::log2((kOverheadSunLux * DegreesSine(20.0f) + kSkyLux) / 2.5f),
                1e-4f);
    EXPECT_NEAR(IncidentLightEv100({.SkyLux = 8600.0f}), std::log2(8600.0f / 2.5f), 1e-4f);
}

// A scene with no sun and no sky has no daylight to read and falls back to a sunlit exterior's EV.
TEST(IncidentLightMeter, NoLightFallsBackToTheStatedEv)
{
    EXPECT_EQ(IncidentLightEv100(IncidentLight{}), Components::kDefaultManualExposureEv);
}

// The reading is not clamped: a moonlit ground (0.25 lx) reads EV100 -3.3, and Auto's Min EV decides
// how much of it a camera shows.
TEST(IncidentLightMeter, AMoonlitReadingIsNotClamped)
{
    EXPECT_NEAR(IncidentLightEv100({.SkyLux = 0.25f}), std::log2(0.25f / 2.5f), 1e-4f);
}

// The meter follows the sun's direction: a sun 30 degrees up delivers half its lux.
TEST(IncidentLightMeter, TheReadingFollowsTheWorldsSun)
{
    FinalizedLights lights;
    const size_t sun = lights.Add(kOverheadSunLux, 90.0f);
    EXPECT_NEAR(ReadEv(lights), std::log2(kOverheadSunLux / 2.5f), 1e-3f);

    FinalizedLights::Aim(lights.At(sun), 30.0f);
    EXPECT_NEAR(ReadEv(lights), std::log2(kOverheadSunLux * 0.5f / 2.5f), 1e-3f)
        << "a sun 30 degrees up delivers half its lux to a horizontal surface";
}

// Every directional light lights the ground: two suns and the sky add on a horizontal surface.
TEST(IncidentLightMeter, TwoDirectionalLightsAddTheirLightOnTheGround)
{
    FinalizedLights lights;
    lights.Add(kOverheadSunLux, 90.0f);
    lights.Add(50000.0f, 30.0f);
    IncidentEnvironment environment{};
    environment.Lit = true;
    environment.Lux = 10000.0f;
    EXPECT_NEAR(ReadEv(lights, environment), std::log2((kOverheadSunLux + 50000.0f * 0.5f + 10000.0f) / 2.5f), 0.01f);
}

// A directional light below the horizon lights no horizontal surface, however bright it is.
TEST(IncidentLightMeter, ALightBelowTheHorizonAddsNothing)
{
    FinalizedLights lights;
    lights.Add(kOverheadSunLux, 90.0f);
    lights.Add(120000.0f, -5.0f);
    EXPECT_NEAR(ReadEv(lights), std::log2(kOverheadSunLux / 2.5f), 0.01f);
}

// A light with Casts Light off adds nothing. (A disabled Light or an inactive entity is never
// extracted, so it never reaches the list.)
TEST(IncidentLightMeter, ALightThatCastsNoLightAddsNothing)
{
    FinalizedLights lights;
    lights.Add(kOverheadSunLux, 90.0f);
    lights.At(lights.Add(200000.0f, 90.0f)).castsLight = 0u;
    EXPECT_NEAR(ReadEv(lights), std::log2(kOverheadSunLux / 2.5f), 0.01f);
}

// The renderer shades the kMaxDirectionalLights strongest directionals and drops the rest; the meter
// reads what is drawn, so a fifth, weakest sun adds nothing.
TEST(IncidentLightMeter, ADirectionalPastTheRenderersCapAddsNothing)
{
    ASSERT_EQ(Engine::Renderer::kMaxDirectionalLights, 4u);
    FinalizedLights lights;
    lights.Add(40000.0f, 90.0f);
    lights.Add(30000.0f, 90.0f);
    lights.Add(20000.0f, 90.0f);
    lights.Add(10000.0f, 90.0f);
    lights.Add(5000.0f, 90.0f);
    EXPECT_NEAR(ReadEv(lights), std::log2(100000.0f / 2.5f), 0.01f);
}

// An environment that lights the scene before its first irradiance reading lands gives no reading,
// never a sun-only value.
TEST(IncidentLightMeter, AnEnvironmentWithoutAReadingGivesNoReading)
{
    FinalizedLights lights;
    lights.Add(kOverheadSunLux, 90.0f);
    IncidentEnvironment pending{};
    pending.Lit = true;
    EXPECT_FALSE(MeasureIncidentLight(lights.Finalize(), pending).has_value());
}

// A reading of L lux from an environment at IBL intensity k adds k x L to the sun's term: the
// light the scene is lit with, not the cube's raw value.
TEST(IncidentLightMeter, TheEnvironmentAddsItsReadingTimesItsIntensity)
{
    FinalizedLights lights;
    lights.Add(kOverheadSunLux, 90.0f);
    IncidentEnvironment environment{};
    environment.Lit = true;
    environment.Lux = 20000.0f;
    environment.IblIntensity = 0.5f;
    EXPECT_NEAR(ReadEv(lights, environment), std::log2((kOverheadSunLux + 0.5f * 20000.0f) / 2.5f), 1e-3f);
}

// The metered luminance the resolve takes is an 18 % card under the reading's light: the resolve's
// key over it is exactly the exposure a Manual camera at that EV gives.
TEST(IncidentLightMeter, TheMeteredLuminanceGivesTheManualExposureAtTheReading)
{
    for (const float ev : {-3.0f, 4.0f, 12.0f, 15.3f})
    {
        const float scale = Rendering::kAutoExposureMiddleGrey / std::exp2(Rendering::MeteredLogLuminance(ev));
        EXPECT_NEAR(std::log2(scale), std::log2(Rendering::EvToLinearExposure(ev)), 1e-5f) << ev;
    }
}
