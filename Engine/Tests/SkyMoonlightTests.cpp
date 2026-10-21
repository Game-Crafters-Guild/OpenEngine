// Moonlight in lux (SkyEnvironment::MoonlightIlluminance): what the light the sky drives delivers at
// full night under a full moon, absolute rather than a fraction of the sun, and what the night sky and
// the moon disc are lit by. The default is a real full moon's 0.25 lx; the phase scales it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "EngineLogCapture.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySystem.h"
#include "SkySunAnchorHarness.h"
#include "Types/ColorUtils.h"

using namespace GameEngine;

namespace
{
constexpr float kMidnightHours = 0.0f;

// A sky linked to one directional light, driving it as it ships (from the light).
struct NightRig
{
    SkySunAnchorHarness Rig;
    ECS::EntityHandle Sun;

    NightRig(float intensity, Components::LightUnit unit, float moonlightLux)
    {
        Sun = Rig.AddDirectional(intensity, unit);
        Components::SkyEnvironment sky{};
        sky.SunLight = Sun;
        sky.MoonlightIlluminance = moonlightLux;
        Rig.SetSky(sky);
    }

    void RunAt(float hours)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        sky.TimeOfDayHours = hours;
        Rig.SetSky(sky);
        Rig.System.Update(Rig.World, 0.0f);
    }

    Components::Light Light() const { return *Rig.World.GetComponent<Components::Light>(Sun); }

    const Rendering::SkySettings& Settings() const
    {
        return Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSettings();
    }

    // The light's colour as the sky's own bodies give it, rebuilt from the rendered sun and moon with
    // the handover the frame used and `moonScale` on the moon.
    void ExpectedColour(float moonScale, float out[3]) const
    {
        const Rendering::SkySystemConfig config{};
        Rendering::SkySystemState state{};
        for (int c = 0; c < 3; ++c)
        {
            state.sunDirWS[c] = Settings().scatteringSunDir[c];
            state.moonDirWS[c] = Settings().moonDirWS[c];
        }
        state.primaryMoonBlend = Rendering::PrimaryMoonBlend(config, state.sunDirWS[1]);
        const float white[3] = {1.0f, 1.0f, 1.0f};
        Rendering::MixLinkedLightGroundColor(
            Rendering::EvaluateBodyGroundColors(Rendering::ScatteringAtmosphere(), config, state, white), moonScale, out);
    }
};
} // namespace

// The default moonlight is a real full moon's: 0.25 lx overhead, so at full night under the default
// full moon the shipped 100 000 lx sun light is the moon's ground colour times the physical
// moon-to-sun ratio, bit for bit, and the moon's own source in the sky is the clear sun's lifted value
// times the same ratio.
TEST(SkyMoonlight, TheDefaultIsARealFullMoon)
{
    NightRig rig(Components::kClearNoonSunIlluminanceLux, Components::LightUnit::Lux,
                 Components::kDefaultMoonlightIlluminanceLux);
    rig.RunAt(kMidnightHours);
    const Rendering::SkySystemConfig config{};
    float expected[3];
    rig.ExpectedColour(config.moonLightIlluminanceScale, expected);
    for (int c = 0; c < 3; ++c)
        EXPECT_EQ(rig.Light().Color[c], expected[c]) << "channel " << c;
    EXPECT_NEAR(Components::kDefaultMoonlightIlluminanceLux, 0.25f, 1e-4f);

    const float clearSun =
        Components::LightIntensityToUnitless(Components::kClearNoonSunIlluminanceLux, Components::LightUnit::Lux) *
        Rendering::SkySourceIlluminanceScale(Rendering::ScatteringAtmosphere());
    EXPECT_NEAR(rig.Settings().moonIntensity, clearSun * config.moonLightIlluminanceScale,
                clearSun * config.moonLightIlluminanceScale * 1e-4f);
}

// Moonlight is absolute: a dimmer sun, or one authored in another unit, has the same night.
TEST(SkyMoonlight, NightIlluminanceIsAbsolute)
{
    NightRig clear(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig soft(20000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig unitless(2.0f, Components::LightUnit::Unitless, Components::kDefaultMoonlightIlluminanceLux);
    clear.RunAt(kMidnightHours);
    soft.RunAt(kMidnightHours);
    unitless.RunAt(kMidnightHours);
    const float night = Components::SkySunIlluminance::DeliveredLux(clear.Light());
    ASSERT_GT(night, 0.01f);
    EXPECT_NEAR(Components::SkySunIlluminance::DeliveredLux(soft.Light()), night, night * 1e-5f);
    EXPECT_NEAR(Components::SkySunIlluminance::DeliveredLux(unitless.Light()), night, night * 1e-5f);
}

// The night sky and the moon disc are lit by the moonlight: twice the moonlight doubles the night
// sky's source and the disc, and dimming the sun moves neither.
TEST(SkyMoonlight, TheNightSkyAndDiscFollowTheMoonlight)
{
    NightRig base(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig brighter(100000.0f, Components::LightUnit::Lux, 2.0f * Components::kDefaultMoonlightIlluminanceLux);
    NightRig dimSun(20000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    for (NightRig* rig : {&base, &brighter, &dimSun})
        rig->RunAt(kMidnightHours);
    EXPECT_NEAR(brighter.Settings().primarySunIntensity / base.Settings().primarySunIntensity, 2.0f, 1e-5f);
    EXPECT_NEAR(brighter.Settings().moonIntensity / base.Settings().moonIntensity, 2.0f, 1e-5f);
    EXPECT_NEAR(dimSun.Settings().primarySunIntensity, base.Settings().primarySunIntensity,
                base.Settings().primarySunIntensity * 1e-6f);
    EXPECT_NEAR(dimSun.Settings().moonIntensity, base.Settings().moonIntensity, base.Settings().moonIntensity * 1e-6f);
}

// By day the moonlight changes nothing: the light and the sky's day source are the same to the bit.
TEST(SkyMoonlight, DaylightIgnoresTheMoonlight)
{
    for (const float hours : {12.0f, 17.5f})
    {
        NightRig base(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
        NightRig bright(100000.0f, Components::LightUnit::Lux, 2000.0f);
        base.RunAt(hours);
        bright.RunAt(hours);
        for (int c = 0; c < 3; ++c)
            EXPECT_EQ(bright.Light().Color[c], base.Light().Color[c]) << "hour " << hours << ", channel " << c;
        EXPECT_EQ(bright.Light().Intensity, base.Light().Intensity) << "hour " << hours;
        EXPECT_EQ(bright.Settings().primarySunIntensity, base.Settings().primarySunIntensity) << "hour " << hours;
    }
}

// No moonlight is a dark night, not an error: the light delivers nothing, every value stays finite, and
// nothing is logged.
TEST(SkyMoonlight, ZeroIsADarkNightNotAnError)
{
    std::vector<std::string> logLines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&logLines, Logger::LogLevel::Warning);
    NightRig rig(100000.0f, Components::LightUnit::Lux, 0.0f);
    rig.RunAt(kMidnightHours);
    for (int c = 0; c < 3; ++c)
        EXPECT_TRUE(std::isfinite(rig.Light().Color[c])) << "channel " << c;
    EXPECT_EQ(Components::SkySunIlluminance::DeliveredLux(rig.Light()), 0.0f);
    EXPECT_TRUE(std::isfinite(rig.Settings().primarySunIntensity));
    EXPECT_TRUE(std::isfinite(rig.Settings().moonIntensity));
    EXPECT_TRUE(logLines.empty()) << logLines.front();
}

// The night sky's own glow at the horizon is the sky's, not the moon's: the same keys under any
// moonlight, so a moonless night keeps its airglow and a bright moon does not brighten it.
TEST(SkyMoonlight, TheNightGlowIsTheSkysOwn)
{
    NightRig base(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig bright(100000.0f, Components::LightUnit::Lux, 2.0f * Components::kDefaultMoonlightIlluminanceLux);
    base.RunAt(kMidnightHours);
    bright.RunAt(kMidnightHours);
    const Components::SkyEnvironment defaults{};
    for (int c = 0; c < 3; ++c)
    {
        EXPECT_EQ(base.Settings().nightSkyHorizonColor[c], defaults.NightSkyHorizonColorKeys.Midnight[c]) << c;
        EXPECT_EQ(bright.Settings().nightSkyHorizonColor[c], defaults.NightSkyHorizonColorKeys.Midnight[c]) << c;
    }
}

// The phase is the moon's place on its cycle: a quarter moon lights the sky and the ground with
// Krisciunas and Schaefer's 0.091 of a full moon's light (2.6 magnitudes fainter), a new moon with
// nothing. The sky's source carries no extinction, so its ratio is the phase law's alone.
TEST(SkyMoonlight, AQuarterMoonGivesTheFractionOfFullThePhaseLawSays)
{
    auto sourceAt = [](float arcPosition)
    {
        NightRig rig(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
        Components::SkyEnvironment sky = rig.Rig.Sky();
        sky.MoonArcPosition = arcPosition;
        rig.Rig.SetSky(sky);
        rig.RunAt(kMidnightHours);
        return rig.Settings().moonIntensity;
    };
    const float full = sourceAt(0.5f);
    ASSERT_GT(full, 0.0f);
    EXPECT_NEAR(sourceAt(0.25f) / full, 0.0910f, 0.0005f);
    EXPECT_NEAR(sourceAt(0.75f) / full, 0.0910f, 0.0005f);
    EXPECT_EQ(sourceAt(0.0f), 0.0f);

    Components::SkyEnvironment fullSky{};
    Components::SkyEnvironment quarterSky{};
    quarterSky.MoonArcPosition = 0.25f;
    EXPECT_NEAR(Components::SkySunDrive::MoonlightLux(quarterSky, 1.0f) /
                    Components::SkySunDrive::MoonlightLux(fullSky, 1.0f),
                0.0910f, 0.0005f);
}
