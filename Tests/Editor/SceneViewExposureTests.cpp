// The Scene View's exposure settings: Auto with the lights meter, a game camera's floor and curve by default,
// the mode and the meter persisted under their own preference keys, and the handoff into Fixed. The
// meter itself is the engine's (IncidentLightMeter, EnginePostProcessTests).

#include <gtest/gtest.h>

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Panels/ExposureReadingFilter.h"
#include "SceneView/SceneViewAutoExposure.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

using namespace GameEngine;
using Editor::SceneViewExposureMode;
using Editor::SceneViewSettings;

namespace
{
// Points the editor preferences at a fresh directory for one test and restores the Scene View's
// exposure state afterward.
class ScopedScenePreferences
{
  public:
    explicit ScopedScenePreferences(const char* name)
        : m_Root(TestUtils::MakeUniqueTempDirectory(name))
    {
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", m_Root.string().c_str());
        const SceneViewSettings& settings = SceneViewSettings::Get();
        m_Mode = settings.GetExposureMode();
        m_Meter = settings.GetExposureMeter();
        m_MinEv = settings.GetPostFxAutoExposureMinEv();
        m_MaxEv = settings.GetPostFxAutoExposureMaxEv();
        m_FixedEv = settings.GetPostFxFixedExposureEv();
    }
    ~ScopedScenePreferences()
    {
        SceneViewSettings& settings = SceneViewSettings::Get();
        settings.SetExposureMode(m_Mode, /*fromUserSwitch=*/false);
        settings.SetExposureMeter(m_Meter);
        settings.SetPostFxAutoExposureMinEv(m_MinEv);
        settings.SetPostFxAutoExposureMaxEv(m_MaxEv);
        settings.SetPostFxFixedExposureEv(m_FixedEv);
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

  private:
    std::filesystem::path m_Root;
    SceneViewExposureMode m_Mode{};
    Components::ExposureMeter m_Meter{};
    float m_MinEv = 0.0f;
    float m_MaxEv = 0.0f;
    float m_FixedEv = 0.0f;
};
} // namespace

// The Scene View meters the lights by default, at once, with a game camera's floor (the slider's
// bottom, down to a moonless night) and its ceiling at the slider's top; a game camera keeps its
// ceiling 18.
TEST(SceneViewExposure, TheDefaultIsAutoWithTheLightsMeterAndTheCamerasFloor)
{
    EXPECT_EQ(SceneViewSettings::kDefaultExposureMode, SceneViewExposureMode::Auto);
    EXPECT_EQ(SceneViewSettings::kDefaultExposureMeter, Components::ExposureMeter::Lights);
    EXPECT_EQ(SceneViewSettings::kDefaultAutoExposureMinEv, SceneViewSettings::kMinExposureEv);
    EXPECT_EQ(SceneViewSettings::kDefaultAutoExposureMinEv, Components::Camera{}.AutoExposureMinEv)
        << "the Scene View and a game camera share one floor";
    EXPECT_EQ(SceneViewSettings::kDefaultAutoExposureMaxEv, SceneViewSettings::kMaxExposureEv);
    EXPECT_FLOAT_EQ(SceneViewSettings::kMinExposureEv, -7.0f) << "the floor reaches a moonless night (about EV100 -7)";
    EXPECT_FLOAT_EQ(SceneViewSettings::kMaxExposureEv, 20.0f);
    EXPECT_FALSE(SceneViewSettings::kDefaultAutoExposureAdaptationDelay)
        << "the Scene View applies each reading at once unless the user turns the delay on";

    const Components::Camera camera{};
    EXPECT_FLOAT_EQ(camera.AutoExposureMaxEv, 18.0f);
}

// The mode and the meter are user preferences, saved by name and read back.
TEST(SceneViewExposure, TheModeAndTheMeterPersistByName)
{
    ScopedScenePreferences scope("SceneViewExposurePersist");
    SceneViewSettings& settings = SceneViewSettings::Get();

    settings.SetExposureMode(SceneViewExposureMode::Fixed, /*fromUserSwitch=*/false);
    settings.SetExposureMeter(Components::ExposureMeter::Picture);
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        ASSERT_TRUE(prefs.Load(&err)) << err;
        std::string stored;
        ASSERT_TRUE(prefs.TryGetString("sceneView.exposure", stored));
        EXPECT_EQ(stored, "fixed");
        ASSERT_TRUE(prefs.TryGetString("sceneView.exposureMeter", stored));
        EXPECT_EQ(stored, "picture");
        prefs.SetString("sceneView.exposure", "auto");
        prefs.SetString("sceneView.exposureMeter", "lights");
        ASSERT_TRUE(prefs.Save(&err)) << err;
    }
    settings.Load();
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Auto);
    EXPECT_EQ(settings.GetExposureMeter(), Components::ExposureMeter::Lights);
}

// The keys of the former three-mode exposure and its 4 / 18 clamps are not read: a preferences file
// that still holds them keeps the current values, not its old ones.
TEST(SceneViewExposure, TheFormerPreferenceKeysAreNotRead)
{
    ScopedScenePreferences scope("SceneViewExposureFormerKeys");
    SceneViewSettings& settings = SceneViewSettings::Get();
    settings.SetExposureMode(SceneViewExposureMode::Auto, /*fromUserSwitch=*/false);
    settings.SetPostFxAutoExposureMinEv(SceneViewSettings::kMinExposureEv);
    settings.SetPostFxAutoExposureMaxEv(SceneViewSettings::kMaxExposureEv);
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        ASSERT_TRUE(prefs.Load(&err)) << err;
        prefs.SetDouble("sceneView.exposureMode", 2.0); // the former Fixed
        prefs.SetDouble("sceneView.postFxAutoExposureMinEv", 4.0);
        prefs.SetDouble("sceneView.postFxAutoExposureMaxEv", 18.0);
        ASSERT_TRUE(prefs.Save(&err)) << err;
    }
    settings.Load();
    EXPECT_EQ(settings.GetExposureMode(), SceneViewExposureMode::Auto);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMinEv(), SceneViewSettings::kMinExposureEv);
    EXPECT_FLOAT_EQ(settings.GetPostFxAutoExposureMaxEv(), SceneViewSettings::kMaxExposureEv);
}

// Switching from Auto to Fixed starts Fixed at the EV the view was showing, on a slider detent, so
// the image does not jump at the switch.
TEST(SceneViewExposure, SwitchingToFixedStartsAtTheShownEv)
{
    ScopedScenePreferences scope("SceneViewExposureHandoff");
    SceneViewSettings& settings = SceneViewSettings::Get();

    settings.SetExposureMode(SceneViewExposureMode::Auto, /*fromUserSwitch=*/false);
    settings.SetPostFxFixedExposureEv(9.0f);
    settings.SetLastShownExposureEv(13.37f);
    settings.SetExposureMode(SceneViewExposureMode::Fixed, /*fromUserSwitch=*/true);
    EXPECT_FLOAT_EQ(settings.GetPostFxFixedExposureEv(), 13.4f);
}

// What the Scene View publishes with its shipped settings: Auto on the Lights meter, the open clamps
// (with no volume clamp, its own floor reaches a moonless night), and both speeds at the instant
// constant, so a new reading lands on its first frame.
TEST(SceneViewExposure, TheDefaultAutoPublishesTheLightsMeterTheFloorAndInstantSpeeds)
{
    Engine::Renderer::PostProcessSettings settings{};
    Editor::ApplySceneViewAutoExposure(Editor::SceneViewAutoExposure{}, settings);
    EXPECT_TRUE(settings.AutoExposureActive);
    EXPECT_EQ(settings.AutoExposureMeter, static_cast<int32>(Components::ExposureMeter::Lights));
    EXPECT_FLOAT_EQ(settings.AutoExposureMinEv, SceneViewSettings::kMinExposureEv)
        << "a view with no volume clamp keeps its own floor";
    EXPECT_FLOAT_EQ(settings.AutoExposureMaxEv, 20.0f);
    EXPECT_EQ(settings.AutoExposureSpeedUp, Editor::kInstantAdaptationSpeed);
    EXPECT_EQ(settings.AutoExposureSpeedDown, Editor::kInstantAdaptationSpeed);
    // The night follows a game camera's default compensation curve.
    const auto curve = Rendering::SampleExposureCompensation(Components::DefaultExposureCompensationCurve());
    EXPECT_EQ(std::memcmp(&settings.AutoExposureCompensation, &curve, sizeof(curve)), 0);

    // The instant speed lands the target on the first frame at a 240 Hz frame time: the resolve's
    // smoothing factor is exactly 1 in float32.
    const float dt = 1.0f / 240.0f;
    EXPECT_EQ(1.0f - std::exp2(-dt * Editor::kInstantAdaptationSpeed), 1.0f);
}

// With the delay on the world sensor's speeds stay, and a volume's Exposure Adjustment clamps narrow
// the Scene View's open envelope; the compensation adds to the bias the volume already set.
TEST(SceneViewExposure, TheDelayKeepsTheSensorSpeedsAndVolumeClampsNarrowTheEnvelope)
{
    Engine::Renderer::PostProcessSettings settings{};
    settings.ExposureClampMinEv = 2.0f;
    settings.ExposureClampMinWeight = 1.0f;
    settings.ExposureClampMaxEv = 16.0f;
    settings.ExposureClampMaxWeight = 1.0f;
    settings.AutoExposureBiasEv = 0.5f;
    const float sensorSpeedUp = settings.AutoExposureSpeedUp;
    Editor::ApplySceneViewAutoExposure(
        {.Meter = Components::ExposureMeter::Picture, .CompensationEv = 1.0f, .AdaptationDelay = true}, settings);
    EXPECT_EQ(settings.AutoExposureMeter, static_cast<int32>(Components::ExposureMeter::Picture));
    EXPECT_FLOAT_EQ(settings.AutoExposureMinEv, 2.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureMaxEv, 16.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureBiasEv, 1.5f);
    EXPECT_EQ(settings.AutoExposureSpeedUp, sensorSpeedUp);
}

// A volume's clamp fades in with its blend weight: one percent into its blend region, a volume that
// clamps the floor at EV100 2 moves the Scene View's -7 floor one percent of the way, not to the
// no-op endpoint at -5; fully inside, the floor is the volume's.
// The Scene View's published floor under one volume that clamps the floor at `authoredMinEv`, blended
// in at `weight` (the clamp and its weight blended from the no-op endpoint and 0, as volumes blend).
static float SceneViewFloorUnderAVolumeClamp(float authoredMinEv, float weight)
{
    Engine::Renderer::PostProcessSettings settings{};
    settings.ExposureClampMinEv = Engine::Renderer::kExposureClampNoOpMinEv +
                                  weight * (authoredMinEv - Engine::Renderer::kExposureClampNoOpMinEv);
    settings.ExposureClampMinWeight = weight;
    Editor::ApplySceneViewAutoExposure(Editor::SceneViewAutoExposure{}, settings);
    return settings.AutoExposureMinEv;
}

TEST(SceneViewExposure, AVolumeFadingInMovesTheFloorSmoothly)
{
    constexpr float kAuthoredMinEv = 2.0f;
    EXPECT_NEAR(SceneViewFloorUnderAVolumeClamp(kAuthoredMinEv, 0.01f), SceneViewSettings::kMinExposureEv + 0.01f * (kAuthoredMinEv - SceneViewSettings::kMinExposureEv),
                1e-4f)
        << "one percent into the blend region the floor moves one percent of the way";
    EXPECT_NEAR(SceneViewFloorUnderAVolumeClamp(kAuthoredMinEv, 1.0f), kAuthoredMinEv, 1e-4f) << "inside the volume the floor is the volume's";
}

// The popover's Reading line follows a live EV100 without flipping on a rounding boundary: a Picture
// reading that hovers across 14.75 refreshes the line once, and a move of a whole display step
// refreshes it again.
TEST(SceneViewExposure, AReadingHoveringAcrossARoundingBoundaryRefreshesTheLineOnce)
{
    Editor::ExposureReadingFilter filter;
    int refreshes = 0;
    for (int frame = 0; frame < 20; ++frame)
        refreshes += filter.Accept(frame % 2 == 0 ? 14.74f : 14.76f) ? 1 : 0;
    EXPECT_EQ(refreshes, 1);
    ASSERT_TRUE(filter.Shown().has_value());
    EXPECT_FLOAT_EQ(*filter.Shown(), 14.74f);

    EXPECT_TRUE(filter.Accept(14.9f)) << "a whole display step away shows the new value";
    EXPECT_FALSE(filter.Accept(std::optional<float>(14.85f)));
    EXPECT_TRUE(filter.Accept(std::nullopt)) << "losing the reading clears the line";
}

// The Scene View takes the curve it is given, the scene's main camera's: a camera with no
// compensation gives a Scene View with none.
TEST(SceneViewExposure, TheSceneViewTakesTheCamerasCurve)
{
    Editor::SceneViewAutoExposure autoExposure{};
    autoExposure.CompensationCurve = Math::Curve{};
    autoExposure.CompensationCurve.TryInsert(Math::CurveKey{-4.0f, -1.0f});
    Engine::Renderer::PostProcessSettings settings{};
    Editor::ApplySceneViewAutoExposure(autoExposure, settings);
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.StartEv, -4.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.Stops[0], -1.0f);
}
