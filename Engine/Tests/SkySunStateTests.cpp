// The sun state the sky publishes each frame (Rendering::SkySunState): the sun's elevation and
// bearing on the template's path, its elevation above the horizon the main camera sees, the moon's
// elevation and phase, and the extraction context carrying it to the volume effects.

#include <gtest/gtest.h>

#include <cmath>

#include "Engine/Rendering/PostProcessVolumeExtract.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySunState.h"
#include "Rendering/Sky/SkySystem.h"
#include "SkySunAnchorHarness.h"

using namespace GameEngine;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegrees = kPi / 180.0;
// A hundredth of a degree, in radians.
constexpr double kHundredthDegree = 0.01 * kDegrees;
// The template's path: latitude 40 north on the March equinox, the SkyEnvironment defaults.
constexpr double kTemplateLatitude = 40.0 * kDegrees;

// The template's sky at `hours`, run for one frame, seen from the views added to it. The constructor
// that takes a height adds one Game view `cameraHeight` meters above the world origin.
struct SunStateRig
{
    SkySunAnchorHarness Rig;

    SunStateRig() { Rig.AddDefaultSky(); }

    explicit SunStateRig(float cameraHeight) : SunStateRig()
    {
        AddView("Game View", Rendering::ViewPurpose::Game, cameraHeight);
    }

    // A view of `purpose` whose camera stands `cameraHeight` meters above the world origin.
    void AddView(const char* name, Rendering::ViewPurpose purpose, float cameraHeight)
    {
        const Rendering::CameraId camera = Rig.Services.Views().AllocateCamera(name);
        Rendering::CameraData data{};
        data.cameraPos[1] = cameraHeight;
        Rig.Services.Views().SetCameraData(camera, data);
        Rig.Services.Views().AllocateView(name, camera, purpose);
    }

    const Rendering::SkySunState& RunAt(float hours, float moonArcPosition = 0.5f)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        sky.TimeOfDayHours = hours;
        sky.MoonArcPosition = moonArcPosition;
        Rig.SetSky(sky);
        Rig.System.Update(Rig.World, 0.0f);
        return Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSunState();
    }
};

// The hour on the template's equinox path at which the sun stands `elevation` above the horizon in
// the morning: sin(elevation) = cos(latitude) sin(theta), theta = (hours / 24 - 1/4) 2 pi.
float MorningHourAtElevation(double elevation)
{
    const double theta = std::asin(std::sin(elevation) / std::cos(kTemplateLatitude));
    return static_cast<float>(6.0 + theta / (2.0 * kPi) * 24.0);
}
} // namespace

// Sunrise, noon and the handover band's upper edge on the template's path: the geometric horizon
// at 06:00 due east, 90 - 40 = 50 degrees due south at noon, and the edge the primary light is the
// sun's alone above (SunOnlyUpDot, 1.15 degrees) at the hour the path reaches it.
TEST(SkySunState, TheSunStandsWhereTheTemplatesPathPutsIt)
{
    SunStateRig rig(0.0f);

    const Rendering::SkySunState sunrise = rig.RunAt(6.0f);
    EXPECT_NEAR(sunrise.SunElevation, 0.0, kHundredthDegree);
    EXPECT_NEAR(sunrise.SunAzimuth, 90.0 * kDegrees, kHundredthDegree);

    const Rendering::SkySunState noon = rig.RunAt(12.0f);
    EXPECT_NEAR(noon.SunElevation, 50.0 * kDegrees, kHundredthDegree);
    EXPECT_NEAR(noon.SunAzimuth, 180.0 * kDegrees, kHundredthDegree);

    const double edge = std::asin(Rendering::SunOnlyUpDot(Rendering::SkySystemConfig{}));
    ASSERT_NEAR(edge, 1.15 * kDegrees, kHundredthDegree);
    const Rendering::SkySunState atEdge = rig.RunAt(MorningHourAtElevation(edge));
    EXPECT_NEAR(atEdge.SunElevation, edge, kHundredthDegree);
    EXPECT_EQ(atEdge.SunApparentElevation, atEdge.SunElevation) << "a camera on the ground sees the geometric horizon";
}

// The horizon dips acos(R / (R + h)) below the horizontal for an eye h above a sphere of radius R:
// about 3.2 degrees at 10 km on the sky's planet, and the sun state's apparent elevation is the
// geometric one plus that dip for the main camera.
TEST(SkySunState, TheHorizonDipsWithTheMainCamerasAltitude)
{
    const float radius = Rendering::ScatteringAtmosphere().planetRadius;
    const double expectedDip = std::acos(static_cast<double>(radius) / (static_cast<double>(radius) + 10000.0));
    EXPECT_NEAR(expectedDip / kDegrees, 3.2, 0.05);

    SunStateRig high(10000.0f);
    const Rendering::SkySunState state = high.RunAt(12.0f);
    EXPECT_NEAR(state.SunElevation, 50.0 * kDegrees, kHundredthDegree);
    EXPECT_NEAR(state.SunApparentElevation - state.SunElevation, expectedDip, kHundredthDegree);
}

// The dip reads the Game view's camera whichever view was allocated first, as the sky-view LUT's
// altitude band does. The editor allocates its Scene view and its Game view in the order they first
// draw.
TEST(SkySunState, TheHorizonDipsForTheGameViewWhicheverViewCameFirst)
{
    const float radius = Rendering::ScatteringAtmosphere().planetRadius;
    const double gameDip = std::acos(static_cast<double>(radius) / (static_cast<double>(radius) + 10000.0));

    SunStateRig sceneFirst;
    sceneFirst.AddView("Scene View", Rendering::ViewPurpose::EditorScene, 0.0f);
    sceneFirst.AddView("Game View", Rendering::ViewPurpose::Game, 10000.0f);
    const Rendering::SkySunState sceneFirstState = sceneFirst.RunAt(12.0f);
    EXPECT_NEAR(sceneFirstState.SunApparentElevation - sceneFirstState.SunElevation, gameDip, kHundredthDegree);

    SunStateRig gameFirst;
    gameFirst.AddView("Game View", Rendering::ViewPurpose::Game, 10000.0f);
    gameFirst.AddView("Scene View", Rendering::ViewPurpose::EditorScene, 0.0f);
    const Rendering::SkySunState gameFirstState = gameFirst.RunAt(12.0f);
    EXPECT_NEAR(gameFirstState.SunApparentElevation - gameFirstState.SunElevation, gameDip, kHundredthDegree);
}

// The moon rides the sun's circle 2 pi * arc position behind it: a full moon (0.5) at midnight
// stands where the noon sun does, opposite it, at the phase law's full fraction; a first quarter
// (0.25) at 18:00 stands due south, a quarter turn from the setting sun, at the law's 0.091.
TEST(SkySunState, TheMoonStandsAndIsLitAsThePhaseLawSays)
{
    SunStateRig rig(0.0f);

    const Rendering::SkySunState full = rig.RunAt(0.0f, 0.5f);
    EXPECT_NEAR(full.MoonElevation, 50.0 * kDegrees, kHundredthDegree);
    EXPECT_FLOAT_EQ(full.MoonPhaseFraction, Rendering::MoonPhaseIlluminanceFraction(0.5f));
    EXPECT_FLOAT_EQ(full.MoonPhaseFraction, 1.0f);
    EXPECT_NEAR(full.MoonSunSeparation, kPi, kHundredthDegree);

    const Rendering::SkySunState quarter = rig.RunAt(18.0f, 0.25f);
    EXPECT_NEAR(quarter.MoonElevation, 50.0 * kDegrees, kHundredthDegree);
    EXPECT_FLOAT_EQ(quarter.MoonPhaseFraction, Rendering::MoonPhaseIlluminanceFraction(0.25f));
    EXPECT_NEAR(quarter.MoonPhaseFraction, 0.0910f, 0.0005f);
    EXPECT_NEAR(quarter.MoonSunSeparation, 0.5 * kPi, kHundredthDegree);
}

// The volume effects read the sun state the sky published from the extraction context; with no
// active sky the context holds the defaults.
TEST(SkySunState, TheExtractionContextCarriesTheSunState)
{
    SunStateRig rig(10000.0f);
    const Engine::Renderer::PostProcessExtractContext before =
        Engine::Renderer::MakePostProcessExtractContext(rig.Rig.World, &rig.Rig.Services);
    const Rendering::SkySunState defaults{};
    EXPECT_EQ(before.SunState.SunElevation, defaults.SunElevation);
    EXPECT_EQ(before.SunState.MoonPhaseFraction, defaults.MoonPhaseFraction);

    const Rendering::SkySunState published = rig.RunAt(18.0f, 0.25f);
    const Engine::Renderer::PostProcessExtractContext context =
        Engine::Renderer::MakePostProcessExtractContext(rig.Rig.World, &rig.Rig.Services);
    EXPECT_EQ(context.SunState.SunElevation, published.SunElevation);
    EXPECT_EQ(context.SunState.SunAzimuth, published.SunAzimuth);
    EXPECT_EQ(context.SunState.SunApparentElevation, published.SunApparentElevation);
    EXPECT_EQ(context.SunState.MoonElevation, published.MoonElevation);
    EXPECT_EQ(context.SunState.MoonPhaseFraction, published.MoonPhaseFraction);
    EXPECT_EQ(context.SunState.MoonSunSeparation, published.MoonSunSeparation);
    EXPECT_NE(context.SunState.SunElevation, defaults.SunElevation);
}
