// The editor's preview hour on the sky system: the frame renders the preview in place of the
// component's hour, in the same frame, per world, with the component and its animated clock left
// alone; and the setting half of the day read from the sun state the sky publishes (IsDuskHalf) for
// a driven sun, a sun set by hand and a polar day.

#include <gtest/gtest.h>

#include <cmath>

#include "Assets/DayCycleProfile.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Sky/SkySunState.h"
#include "SkySunAnchorHarness.h"

using namespace GameEngine;

namespace
{
constexpr double kDegrees = 3.14159265358979323846 / 180.0;
// A thousandth of a degree, in radians.
constexpr float kTolerance = static_cast<float>(0.001 * kDegrees);

struct PreviewRig
{
    SkySunAnchorHarness Rig;

    explicit PreviewRig(float hours, bool animate = false)
    {
        Rig.AddDefaultSky();
        Components::SkyEnvironment sky = Rig.Sky();
        sky.TimeOfDayHours = hours;
        sky.AnimateTimeOfDay = animate;
        Rig.SetSky(sky);
    }

    const Rendering::SkySunState& Run(float deltaSeconds = 0.0f)
    {
        Rig.System.Update(Rig.World, deltaSeconds);
        return Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSunState();
    }

    uint64 WorldId() const { return Rig.World.GetWorldId(); }
};

// The sun state of the default sky at `hours`, rendered from its component.
Rendering::SkySunState StateAt(float hours)
{
    PreviewRig reference(hours);
    return reference.Run();
}
} // namespace

TEST(SkyPreviewHour, TheFrameRendersThePreviewHourInPlaceOfTheComponents)
{
    PreviewRig rig(10.0f);
    rig.Rig.System.SetPreviewHour(rig.WorldId(), 18.5f);
    const Rendering::SkySunState state = rig.Run();
    const Rendering::SkySunState dusk = StateAt(18.5f);
    EXPECT_NEAR(state.SunElevation, dusk.SunElevation, kTolerance);
    EXPECT_NEAR(state.SunAzimuth, dusk.SunAzimuth, kTolerance);
    EXPECT_LT(state.SunElevation, 0.0f) << "18:30 at the template is civil twilight";
    EXPECT_EQ(rig.Rig.Sky().TimeOfDayHours, 10.0f) << "the preview wrote the component";
}

TEST(SkyPreviewHour, ClearingThePreviewReturnsTheComponentsHour)
{
    PreviewRig rig(10.0f);
    rig.Rig.System.SetPreviewHour(rig.WorldId(), 18.5f);
    rig.Run();
    rig.Rig.System.ClearPreviewHour(rig.WorldId());
    EXPECT_FALSE(rig.Rig.System.GetPreviewHour(rig.WorldId()).has_value());
    EXPECT_NEAR(rig.Run().SunElevation, StateAt(10.0f).SunElevation, kTolerance);
}

// Under AnimateTimeOfDay the clock holds while a preview is set and runs on once it is cleared.
TEST(SkyPreviewHour, APreviewHoldsTheAnimatedClock)
{
    PreviewRig rig(10.0f, true);
    rig.Rig.System.SetPreviewHour(rig.WorldId(), 18.5f);
    rig.Run(5.0f);
    EXPECT_EQ(rig.Rig.Sky().TimeOfDayHours, 10.0f);
    rig.Rig.System.ClearPreviewHour(rig.WorldId());
    rig.Run(5.0f);
    EXPECT_GT(rig.Rig.Sky().TimeOfDayHours, 10.0f);
}

TEST(SkyPreviewHour, APreviewBelongsToOneWorld)
{
    PreviewRig previewed(10.0f);
    previewed.Rig.System.SetPreviewHour(previewed.WorldId(), 18.5f);

    // A second world run through the same system keeps its own hour.
    ECS::World other;
    const ECS::EntityHandle sky = other.CreateEntity();
    other.AddComponentImmediate(sky, Components::SkyEnvironment{});
    previewed.Rig.System.Update(other, 0.0f);
    const Rendering::SkySunState otherState =
        previewed.Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSunState();
    EXPECT_NEAR(otherState.SunElevation, StateAt(10.0f).SunElevation, kTolerance);
    EXPECT_NEAR(previewed.Run().SunElevation, StateAt(18.5f).SunElevation, kTolerance);
}

TEST(SkyDuskHalf, ADrivenSunSetsInTheAfternoon)
{
    EXPECT_TRUE(IsDuskHalf(StateAt(17.0f)));
    EXPECT_TRUE(IsDuskHalf(StateAt(18.5f)));
    EXPECT_FALSE(IsDuskHalf(StateAt(7.0f)));
    EXPECT_FALSE(IsDuskHalf(StateAt(5.5f)));
}

// A sun set by hand has no hour: the half comes from the azimuth of its direction.
TEST(SkyDuskHalf, ASunSetByHandTakesTheHalfOfItsDirection)
{
    for (const float hours : {7.0f, 17.0f})
    {
        PreviewRig driven(hours);
        driven.Run();
        const float* direction =
            driven.Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSettings().primarySunDir;

        PreviewRig byHand(12.0f);
        Components::SkyEnvironment sky = byHand.Rig.Sky();
        sky.AutoSunMoon = false;
        sky.SunDirOverride[0] = direction[0];
        sky.SunDirOverride[1] = direction[1];
        sky.SunDirOverride[2] = direction[2];
        byHand.Rig.SetSky(sky);
        EXPECT_EQ(IsDuskHalf(byHand.Run()), hours > 12.0f) << "the sun of " << hours << " h set by hand";
    }
}

// Latitude 80 north at the June solstice: the sun never sets, and its afternoon is the setting half.
TEST(SkyDuskHalf, APolarDaySplitsAtNoonAndMidnight)
{
    for (const float hours : {1.0f, 11.0f, 13.0f, 23.0f})
    {
        PreviewRig rig(hours);
        Components::SkyEnvironment sky = rig.Rig.Sky();
        sky.Latitude = 80.0f;
        sky.DayOfYear = 172;
        rig.Rig.SetSky(sky);
        const Rendering::SkySunState state = rig.Run();
        EXPECT_GT(state.SunElevation, 0.0f) << hours << " h: the polar sun is up";
        EXPECT_EQ(IsDuskHalf(state), hours > 12.0f) << hours << " h";
    }
}
