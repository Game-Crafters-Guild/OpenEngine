#include <gtest/gtest.h>

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h"
#include "Engine/Rendering/CameraExposureHelpers.h"
#include "../Source/Engine/Rendering/Systems/CameraExposureStamp.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace GameEngine;
using GameEngine::Components::ExposureMode;

// Fixed mode is the legacy linear multiplier; the default (1.0, no compensation) must resolve to
// EXACTLY 1.0 so the render graph's exposure==1.0 tonemap-skip fast path still fires.
TEST(Exposure, FixedModeIsLegacyLinearIdentity)
{
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, 15.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 2.0f, 15.0f, 0.0f), 2.0f);
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 0.5f, 7.0f, 0.0f), 0.5f);
}

// Compensation is +/- stops in BOTH modes, and + brightens (the intuitive artist convention).
TEST(Exposure, CompensationStopsBrightenInBothModes)
{
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, 0.0f, 1.0f), 2.0f);
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, 0.0f, -1.0f), 0.5f);
    // Manual: +1 stop on top of the neutral EV also doubles.
    const float neutral = Rendering::kNeutralExposureEV;
    EXPECT_NEAR(Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral, 1.0f), 2.0f, 1e-4f);
}

// Manual EV100 at the neutral anchor (log2(203)) is the identity multiplier — reference white
// (203 nits == scene-linear 1.0) maps to display white.
TEST(Exposure, ManualNeutralEvIsIdentity)
{
    const float neutral = Rendering::kNeutralExposureEV;
    EXPECT_NEAR(neutral, std::log2(Components::kReferenceWhiteNits), 1e-4f);
    EXPECT_NEAR(Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral, 0.0f), 1.0f, 1e-5f);
    // Brighter scene -> higher EV -> smaller multiplier (photographic convention): +1 EV halves.
    EXPECT_NEAR(Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral + 1.0f, 0.0f), 0.5f, 1e-5f);
}

// The realistic-mapping check: a physical noon sun (~100000 lux) drives a white Lambertian to
// ~157 scene-linear; exposing at the real-world sunny-16 EV100 of ~15 must bring it to ~display
// white. This is what makes the physical light units authorable with real EV numbers.
TEST(Exposure, SunnySceneAtEv15LandsNearDisplayWhite)
{
    const float sunUnitless = 100000.0f * Components::kUnitlessPerLux; // lux -> unitless (~492)
    const float whiteCardSceneLinear = sunUnitless * (1.0f / 3.14159265f); // Lambertian albedo 1, NdotL 1
    const float exposed = whiteCardSceneLinear * Rendering::EvToLinearExposure(15.0f);
    EXPECT_NEAR(exposed, 1.0f, 0.25f) << "sunny-16 EV100=15 should expose a white card near display white";
}

// Forward/inverse must round-trip so the inspector can display the EV of a resolved linear scale.
TEST(Exposure, EvLinearRoundTrip)
{
    for (float ev : {0.0f, 3.0f, 7.66f, 11.0f, 15.0f})
        EXPECT_NEAR(Rendering::LinearExposureToEv(Rendering::EvToLinearExposure(ev)), ev, 1e-3f);
}

// Physical camera: the sunny-16 rule (f/16, 1/100 s, ISO 100) is EV100 ~ 14.6, and each photographic
// control moves the EV by the expected number of stops.
TEST(Exposure, PhysicalCameraEv100MatchesSunny16AndStops)
{
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f, 0.01f, 100.0f), std::log2(25600.0f), 1e-4f); // ~14.64
    const float base = Rendering::PhysicalCameraEv100(16.0f, 0.01f, 100.0f);
    // Open the aperture one stop (area x2 => f /= sqrt(2)) -> 1 stop brighter scene-meter -> EV -1.
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f / std::sqrt(2.0f), 0.01f, 100.0f), base - 1.0f, 1e-4f);
    // Double the shutter time -> EV -1; double the ISO -> EV -1.
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f, 0.02f, 100.0f), base - 1.0f, 1e-4f);
    EXPECT_NEAR(Rendering::PhysicalCameraEv100(16.0f, 0.01f, 200.0f), base - 1.0f, 1e-4f);
}

// Auto-exposure gate + EV-range -> linear clamp inversion. The resolve shader clamps the target
// scale to [minExposure, maxExposure]; minExposure comes from MaxEv (higher EV -> smaller scale).
TEST(Exposure, AutoExposureGateAndClampInversion)
{
    Engine::Renderer::PostProcessSettings s{};
    EXPECT_FALSE(s.IsAutoExposureActive()); // default: off
    s.AutoExposureActive = true;
    EXPECT_TRUE(s.IsAutoExposureActive());

    // The EV range maps to the linear clamp with min < max and both positive.
    const float minClamp = Rendering::EvToLinearExposure(s.AutoExposureMaxEv); // from Max EV
    const float maxClamp = Rendering::EvToLinearExposure(s.AutoExposureMinEv); // from Min EV
    EXPECT_GT(minClamp, 0.0f);
    EXPECT_LT(minClamp, maxClamp);
}

// Grey-card calibration anchor for Phase S (sky physical anchoring). Pins the exact scene-linear
// values the runtime grey-card scene is checked against at a NEUTRAL exposure (Manual EV=log2(203),
// scale 1.0): a 203-nit emitter reads display-white 1.0, and an 18% grey Lambertian facing a 203-lux
// sun reads 0.18/pi. The emissive/grey RATIO (pi/0.18 ~ 17.45) is unit-free, so the runtime test can
// compare it even when absolute HDR capture is unavailable. A sky lit grey card must land a plausible
// sub-sun level against THIS sun-grey to be coherent on the 203 scale.
TEST(Exposure, GreyCardCalibrationAnchorAtNeutralExposure)
{
    const float neutral = Rendering::kNeutralExposureEV;
    const float scale = Rendering::ResolveExposureScale(ExposureMode::Manual, 1.0f, neutral, 0.0f);
    EXPECT_NEAR(scale, 1.0f, 1e-5f);

    // A 203-nit emitter is scene-linear 1.0 by the emission anchor (surface_io.glsl: nits / 203);
    // exposed at the neutral scale it stays display white. (Folding the runtime scale through keeps
    // these as real value checks, not compile-time-constant comparisons.)
    const float emissiveExposed = scale;
    EXPECT_NEAR(emissiveExposed, 1.0f, 1e-5f);

    // 18% grey Lambertian, normal-facing a 203-lux sun (NdotL 1): albedo/pi * unitless(203 lx == 1.0).
    const float sunUnitless = Components::LightIntensityToUnitless(203.0f, Components::LightUnit::Lux);
    const float greyCardExposed = 0.18f * (1.0f / 3.14159265f) * sunUnitless * scale;
    EXPECT_NEAR(greyCardExposed, 0.0572958f, 1e-4f);

    // The emissive/grey ratio the runtime calibration compares: a sky-lit grey must land a plausible
    // sub-sun fraction of this sun-lit grey to be coherent on the 203 scale.
    EXPECT_NEAR(emissiveExposed / greyCardExposed, 3.14159265f / 0.18f, 1e-2f);
}

// The default auto-metering bias ships 0: reference-photographic metering through the untouched
// Hill fit (AE-ACES content renders ~0.85 stops darker than the pre-flip 1.8-pre-scale look; the
// shipped value is a pending pick, compared live via Exposure Compensation +0.85 — see
// kDefaultAutoExposureBiasEv). Reference relation kept as documentation: a bias of exactly
// log2(1.8) ~ +0.85 commutes with the removed ACES input pre-scale, so THAT bias reproduces the
// established pre-flip brightness. Guard the struct default so any Auto path that never runs the
// extraction stamp still lands on the shipped anchor.
TEST(Exposure, DefaultAutoExposureBiasIsReferencePhotographic)
{
    EXPECT_FLOAT_EQ(Engine::Renderer::kDefaultAutoExposureBiasEv, 0.0f);
    // Provenance of the +0.85 comparison lever: it is the removed 1.8 pre-scale in stops.
    EXPECT_NEAR(0.85f, std::log2(1.8f), 0.01f);
    Engine::Renderer::PostProcessSettings s{};
    EXPECT_FLOAT_EQ(s.AutoExposureBiasEv, Engine::Renderer::kDefaultAutoExposureBiasEv);
}

// TryWriteField folds the bias into the metering key AND both envelope clamps by the same factor.
// Scaling all three is exactly a post-clamp multiply of the resolved exposure — the UE semantics:
// compensation stays effective even when adaptation is pinned at the MinEv/MaxEv envelope edge
// (the old key-only fold went dead there).
TEST(Exposure, AutoExposureBiasFoldsIntoKeyAndBothClamps)
{
    using GameEngine::Rendering::ShaderMeta;
    using GameEngine::Rendering::NamedPushConstantWriter;

    ShaderMeta meta{};
    GameEngine::Rendering::PushConstantRangeMeta pc{};
    pc.Name = "PC";
    pc.Size = 12;
    pc.Block.Size = 12;
    const char* memberNames[3] = {"exposureKey", "minExposure", "maxExposure"};
    for (uint32_t i = 0; i < 3; ++i)
    {
        GameEngine::Rendering::Member m{};
        m.Name = memberNames[i];
        m.Offset = i * 4;
        m.Size = 4;
        pc.Block.Members.push_back(m);
    }
    meta.PushConstants.push_back(pc);

    Engine::Renderer::PostProcessSettings s{};
    s.AutoExposureActive = true;
    s.AutoExposureBiasEv = 0.85f;

    NamedPushConstantWriter pcw(meta, "PC");
    ASSERT_TRUE(pcw.IsValid());
    for (const char* name : memberNames)
        ASSERT_TRUE(s.TryWriteField(name, pcw)) << name;

    float key = 0.0f, minExp = 0.0f, maxExp = 0.0f;
    std::memcpy(&key, pcw.GetBuffer().data() + 0, 4);
    std::memcpy(&minExp, pcw.GetBuffer().data() + 4, 4);
    std::memcpy(&maxExp, pcw.GetBuffer().data() + 8, 4);

    const float bias = std::exp2(s.AutoExposureBiasEv);
    EXPECT_NEAR(key, Rendering::kAutoExposureMiddleGrey * bias, 1e-6f);
    EXPECT_NEAR(minExp, Rendering::EvToLinearExposure(s.AutoExposureMaxEv) * bias, 1e-8f);
    EXPECT_NEAR(maxExp, Rendering::EvToLinearExposure(s.AutoExposureMinEv) * bias, 1e-4f);

    // The post-clamp identity the fold relies on: with the resolve shader's
    // clamp(key/avg, lo, hi) evaluated on the folded values, a target pinned at either
    // envelope edge still carries the full bias.
    const float lo = Rendering::EvToLinearExposure(s.AutoExposureMaxEv);
    const float hi = Rendering::EvToLinearExposure(s.AutoExposureMinEv);
    const float brightAvg = 1000.0f; // pins at lo
    const float darkAvg = 1e-9f;     // pins at hi
    EXPECT_NEAR(std::clamp(key / brightAvg, minExp, maxExp), lo * bias, lo * bias * 1e-4f);
    EXPECT_NEAR(std::clamp(key / darkAvg, minExp, maxExp), hi * bias, hi * bias * 1e-4f);
}

// The bias participates in the volume blend like the other EV-space exposure fields.
TEST(Exposure, AutoExposureBiasBlendsAcrossVolumes)
{
    Engine::Renderer::PostProcessSettings a{}, b{};
    a.AutoExposureBiasEv = 0.85f;
    b.AutoExposureBiasEv = 2.0f;
    const auto out = Engine::Renderer::BlendPostProcessSettings(
        a, b, 1.0f, Engine::Renderer::SettingsBlendGroupMask::All());
    EXPECT_FLOAT_EQ(out.AutoExposureBiasEv, 2.0f);
    const auto mid = Engine::Renderer::BlendPostProcessSettings(
        a, b, 0.5f, Engine::Renderer::SettingsBlendGroupMask::All());
    EXPECT_NEAR(mid.AutoExposureBiasEv, 0.5f * (0.85f + 2.0f), 1e-5f);
}

// Physical mode routes the camera EV through the same EV-based path as Manual, so a sunny-16 camera
// exposes a physical noon sun to a bright (near display white) value rather than blowing out.
TEST(Exposure, PhysicalModeExposesSunnySceneSanely)
{
    const float ev = Rendering::ExposureEv100ForMode(ExposureMode::Physical, 0.0f, 16.0f, 0.01f, 100.0f);
    const float scale = Rendering::ResolveExposureScale(ExposureMode::Physical, 1.0f, ev, 0.0f);
    const float sunUnitless = 100000.0f * Components::kUnitlessPerLux;
    const float whiteCard = sunUnitless * (1.0f / 3.14159265f) * scale;
    EXPECT_GT(whiteCard, 0.4f);  // not crushed to black
    EXPECT_LT(whiteCard, 4.0f);  // not blown out by orders of magnitude (bright, near/above white)
    // Fixed mode ignores the camera EV entirely (back-compat).
    EXPECT_FLOAT_EQ(Rendering::ResolveExposureScale(ExposureMode::Fixed, 1.0f, ev, 0.0f), 1.0f);
}

// The camera component and the render-side camera exposure must start from the one shared
// default, so a camera switched to Manual exposes exactly like the Scene View it was framed in.
// The editor's Scene View pin reads the same constant but is not observable from an engine test:
// it is a private field on a prefs-backed singleton with no reset API, so a test would read
// whatever the user's preferences hold rather than the default. Nothing here sees that half.
TEST(Exposure, ManualEvDefaultIsSharedAcrossCameraAndViewRegistry)
{
    EXPECT_FLOAT_EQ(Components::Camera{}.ManualExposureEV, Components::kDefaultManualExposureEv);
    EXPECT_FLOAT_EQ(Engine::Renderer::ViewRegistry::CameraExposure{}.ManualExposureEV,
                    Components::kDefaultManualExposureEv);
    // Auto stays the camera's default mode: the shared EV is the fallback a manual camera lands on,
    // not a switch to manual exposure.
    EXPECT_EQ(Components::Camera{}.ExposureControl, ExposureMode::Auto);
}

// A game camera meters the picture by default with a floor that reaches a moonlit night and its
// ceiling, and the meter it is set to reaches the view's exposure (the field the per-view resolve stamps).
TEST(Exposure, TheCameraMetersThePictureByDefaultAndPublishesItsMeter)
{
    const Components::Camera defaults{};
    EXPECT_EQ(defaults.AutoExposureMeter, Components::ExposureMeter::Picture);
    EXPECT_FLOAT_EQ(defaults.AutoExposureMinEv, -7.0f);
    EXPECT_FLOAT_EQ(defaults.AutoExposureMaxEv, 18.0f);
    EXPECT_EQ(Engine::Renderer::ToCameraExposure(defaults).AutoExposureMeter,
              static_cast<int32>(Components::ExposureMeter::Picture));

    Components::Camera lights{};
    lights.AutoExposureMeter = Components::ExposureMeter::Lights;
    EXPECT_EQ(Engine::Renderer::ToCameraExposure(lights).AutoExposureMeter,
              static_cast<int32>(Components::ExposureMeter::Lights));
}

// One floor for every view: the camera, the render-side copy a camera publishes, and the settings a
// view starts from.
TEST(Exposure, TheAutoExposureFloorIsSharedAcrossCameraViewRegistryAndSettings)
{
    EXPECT_FLOAT_EQ(Components::kDefaultAutoExposureMinEv, -7.0f);
    EXPECT_FLOAT_EQ(Components::Camera{}.AutoExposureMinEv, Components::kDefaultAutoExposureMinEv);
    EXPECT_FLOAT_EQ(Engine::Renderer::ViewRegistry::CameraExposure{}.AutoExposureMinEv,
                    Components::kDefaultAutoExposureMinEv);
    EXPECT_FLOAT_EQ(Engine::Renderer::PostProcessSettings{}.AutoExposureMinEv, Components::kDefaultAutoExposureMinEv);
}

namespace
{
bool SameKey(const Math::CurveKey& a, const Math::CurveKey& b)
{
    return a.Time == b.Time && a.Value == b.Value && a.Interp == b.Interp && a.TangentMode == b.TangentMode;
}

// The samples read the way auto_exposure_resolve.comp reads them: linear between samples, the end
// samples outside.
float InterpolateSamples(const Engine::Renderer::ExposureCompensationSamples& samples, float ev)
{
    using Samples = Engine::Renderer::ExposureCompensationSamples;
    const float span = samples.EndEv - samples.StartEv;
    const float perStop = span > 1.0e-4f ? static_cast<float>(Samples::kCount - 1) / span : 0.0f;
    const float t = std::clamp((ev - samples.StartEv) * perStop, 0.0f, static_cast<float>(Samples::kCount - 1));
    const uint32 i = std::min(static_cast<uint32>(t), Samples::kCount - 2);
    return samples.Stops[i] + (samples.Stops[i + 1] - samples.Stops[i]) * (t - static_cast<float>(i));
}
} // namespace

// The default compensation curve, pinned: two smooth keys with flat tangents, 0 from EV100 0.5 up and
// -2.5 from the moonlit -4.25 down.
TEST(ExposureCompensationCurve, TheDefaultKeysArePinned)
{
    using Math::CurveInterp;
    using Math::CurveTangentMode;
    const Math::Curve curve = Components::DefaultExposureCompensationCurve();
    ASSERT_EQ(curve.KeyCount, 2u);
    EXPECT_TRUE(SameKey(curve.Keys[0], {-4.25f, -2.5f, 0.0f, 0.0f, CurveInterp::Smooth, CurveTangentMode::Flat}));
    EXPECT_TRUE(SameKey(curve.Keys[1], {0.5f, 0.0f, 0.0f, 0.0f, CurveInterp::Smooth, CurveTangentMode::Flat}));
    EXPECT_TRUE(SameKey(Components::Camera{}.AutoExposureCompensationCurve.Keys[1], curve.Keys[1]));
    EXPECT_EQ(Engine::Renderer::ViewRegistry::CameraExposure{}.AutoExposureCompensationCurve.KeyCount, 2u);
}

// The default never brightens and leaves day, shade, dusk and a lamp-lit room (EV100 0.5 and up) as
// metered. A darker scene never shows brighter: the curve never falls as the scene brightens.
TEST(ExposureCompensationCurve, TheDefaultLeavesTheDayAndNeverShowsADarkerSceneBrighter)
{
    const Math::Curve curve = Components::DefaultExposureCompensationCurve();
    const float floorEv = Components::kDefaultAutoExposureMinEv;
    float previousShown = -16.0f + curve.Evaluate(-16.0f) - floorEv;
    for (float ev = -16.0f; ev <= 20.0f; ev += 0.05f)
    {
        const float stops = curve.Evaluate(ev);
        EXPECT_LE(stops, 0.0f) << "at EV100 " << ev;
        // How bright middle gray shows, in stops, relative to the floor: the reading minus the
        // exposure the clamp holds, plus the curve.
        const float shown = ev - std::max(ev, floorEv) + stops;
        EXPECT_GE(shown, previousShown - 1.0e-4f) << "at EV100 " << ev;
        previousShown = shown;
    }
    EXPECT_FLOAT_EQ(curve.Evaluate(0.5f), 0.0f);
    EXPECT_FLOAT_EQ(curve.Evaluate(0.8f), 0.0f) << "a lamp-lit room";
    EXPECT_FLOAT_EQ(curve.Evaluate(13.0f), 0.0f) << "a shaded day";
    EXPECT_FLOAT_EQ(curve.Evaluate(-4.25f), -2.5f) << "a full moon";
    EXPECT_FLOAT_EQ(curve.Evaluate(-11.0f), -2.5f) << "a moonless night";
}

// The samples span the keys, through the one curve evaluator, and an empty curve adds nothing.
TEST(ExposureCompensationCurve, SamplesSpanTheKeysThroughTheCurveEvaluator)
{
    using Samples = Engine::Renderer::ExposureCompensationSamples;
    const Math::Curve curve = Components::DefaultExposureCompensationCurve();
    const Samples samples = Rendering::SampleExposureCompensation(curve);
    EXPECT_FLOAT_EQ(samples.StartEv, -4.25f);
    EXPECT_FLOAT_EQ(samples.EndEv, 0.5f);
    for (uint32 i = 0; i < Samples::kCount; ++i)
        EXPECT_NEAR(samples.Stops[i], curve.Evaluate(-4.25f + 4.75f * static_cast<float>(i) / 15.0f), 1.0e-5f) << i;

    const Samples empty = Rendering::SampleExposureCompensation(Math::Curve{});
    for (float stops : empty.Stops)
        EXPECT_EQ(stops, 0.0f);

    Math::Curve single{};
    single.TryInsert(Math::CurveKey{3.0f, -1.5f});
    const Samples constant = Rendering::SampleExposureCompensation(single);
    EXPECT_FLOAT_EQ(InterpolateSamples(constant, -20.0f), -1.5f);
    EXPECT_FLOAT_EQ(InterpolateSamples(constant, 20.0f), -1.5f);
}

// The resolve's linear read of the samples stays within a twentieth of a stop of the default curve
// at every reading, so the sampling does not show.
TEST(ExposureCompensationCurve, TheSampledDefaultFollowsTheCurve)
{
    const Math::Curve curve = Components::DefaultExposureCompensationCurve();
    const auto samples = Rendering::SampleExposureCompensation(curve);
    for (float ev = -16.0f; ev <= 20.0f; ev += 0.05f)
        EXPECT_NEAR(InterpolateSamples(samples, ev), curve.Evaluate(ev), 0.05f) << "at EV100 " << ev;
}

// A camera's curve reaches the render-side copy it publishes.
TEST(ExposureCompensationCurve, TheCameraPublishesItsCurve)
{
    Components::Camera camera{};
    camera.AutoExposureCompensationCurve = Math::Curve{};
    camera.AutoExposureCompensationCurve.TryInsert(Math::CurveKey{0.0f, -1.0f});
    const auto published = Engine::Renderer::ToCameraExposure(camera);
    ASSERT_EQ(published.AutoExposureCompensationCurve.KeyCount, 1u);
    EXPECT_FLOAT_EQ(published.AutoExposureCompensationCurve.Keys[0].Value, -1.0f);
}

// A view with no camera takes the default sensor: Auto, the shared floor and the default curve.
TEST(ExposureCompensationCurve, AViewWithNoCameraTakesTheDefaultCurve)
{
    Engine::Renderer::PostProcessSettings settings{};
    Engine::Renderer::ApplyCameraExposure(settings, Engine::Renderer::ViewRegistry::CameraExposure{});
    EXPECT_TRUE(settings.AutoExposureActive);
    EXPECT_FLOAT_EQ(settings.AutoExposureMinEv, Components::kDefaultAutoExposureMinEv);
    const auto expected = Rendering::SampleExposureCompensation(Components::DefaultExposureCompensationCurve());
    EXPECT_EQ(std::memcmp(&settings.AutoExposureCompensation, &expected, sizeof(expected)), 0);
}

// A camera's own curve reaches its view's settings; a camera with no keys adds nothing.
TEST(ExposureCompensationCurve, TheCameraCurveReachesItsView)
{
    Components::Camera camera{};
    camera.AutoExposureCompensationCurve = Math::Curve{};
    camera.AutoExposureCompensationCurve.TryInsert(Math::CurveKey{-6.0f, -2.0f});
    camera.AutoExposureCompensationCurve.TryInsert(Math::CurveKey{2.0f, 0.0f});
    Engine::Renderer::PostProcessSettings settings{};
    Engine::Renderer::ApplyCameraExposure(settings, Engine::Renderer::ToCameraExposure(camera));
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.StartEv, -6.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.EndEv, 2.0f);
    EXPECT_FLOAT_EQ(settings.AutoExposureCompensation.Stops[0], -2.0f);

    camera.AutoExposureCompensationCurve = Math::Curve{};
    Engine::Renderer::PostProcessSettings flat{};
    Engine::Renderer::ApplyCameraExposure(flat, Engine::Renderer::ToCameraExposure(camera));
    for (float stops : flat.AutoExposureCompensation.Stops)
        EXPECT_EQ(stops, 0.0f);
}

// A volume's Clamp Min starts at the camera's floor: ticking it changes nothing until the author drags
// it, as Clamp Max starts at the camera's ceiling.
TEST(Exposure, TheVolumeClampsStartAtTheCamerasFloorAndCeiling)
{
    EXPECT_FLOAT_EQ(Components::ExposureAdjustmentEffect{}.MinEv, Components::Camera{}.AutoExposureMinEv);
    EXPECT_FLOAT_EQ(Components::ExposureAdjustmentEffect{}.MaxEv, Components::Camera{}.AutoExposureMaxEv);
}
