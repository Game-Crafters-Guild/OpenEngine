#include <gtest/gtest.h>

#include "CBTTerrain/CBTPlanetShading.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceRuleEval.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceRulePresets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::TerrainECS;
using Mathematics::Vector3;
using Mathematics::WorldPosition;

namespace
{

constexpr float64 kPlanetRadius = 600000.0;
constexpr float32 kDegreesToRadians = std::numbers::pi_v<float32> / 180.0f;
// The base relief of the test planet: its ground spans -/+ 1.5 x 2000 m about the radius.
constexpr float32 kAmplitude = 2000.0f;
// Every measurement of a planet rule sample.
constexpr TerrainRuleMeasurements kEveryMeasurement{};

TerrainHeightRangeMeters TestPlanetRange()
{
    TerrainPlanetRelief relief{};
    relief.Amplitude = kAmplitude;
    return PlanetHeightRangeMeters(relief);
}

// The point at `radius` along `dir`, normalized in doubles so the radius is exact.
WorldPosition PointAt(const Vector3& dir, float64 radius)
{
    const float64 length = std::sqrt(static_cast<float64>(dir.x) * dir.x + static_cast<float64>(dir.y) * dir.y +
                                     static_cast<float64>(dir.z) * dir.z);
    const float64 scale = radius / length;
    return WorldPosition{dir.x * scale, dir.y * scale, dir.z * scale};
}

// The unit direction at a latitude and longitude, in degrees, about +Y.
Vector3 DirectionAt(float32 latitudeDegrees, float32 longitudeDegrees)
{
    const float32 lat = latitudeDegrees * kDegreesToRadians;
    const float32 lon = longitudeDegrees * kDegreesToRadians;
    return Vector3(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
}

// `dir` tilted by `tiltDegrees` toward a tangent: a normal at that slope.
Vector3 TiltedNormal(const Vector3& dir, float32 tiltDegrees)
{
    const Vector3 tangent = CBTTerrain::AnyTangent(dir);
    const float32 tilt = tiltDegrees * kDegreesToRadians;
    return (dir * std::cos(tilt) + tangent * std::sin(tilt)).Normalize();
}

float32 PlanetNoise(const WorldPosition& position, float32 frequency, uint32 seed)
{
    return PlanetRuleNoiseSample(position, frequency, seed);
}

// The Replace arm of CompositeSplatTexel in float: each row lerps every channel toward its
// pure material by the row's weight; the empty texel is the first material.
std::array<float32, 4> ComposePlanetRows(const TerrainSurfaceRulesEffect& fx, const WorldPosition& position,
                                         const TerrainRuleSample& sample)
{
    std::array<float32, 4> weights{};
    for (uint32 r = 0; r < fx.RuleCount; ++r)
    {
        const TerrainSurfaceRule& rule = fx.Rules[r];
        EXPECT_TRUE(rule.Replace) << "row " << r;
        const float32 w = EvaluateTerrainSurfaceRuleWeight(
            rule, sample, [&](float32, float32, float32 frequency, uint32 seed) {
                return PlanetNoise(position, frequency, seed);
            });
        if (w > 0.0f && weights[0] == 0.0f && weights[1] == 0.0f && weights[2] == 0.0f && weights[3] == 0.0f)
            weights[0] = 1.0f;
        for (uint32 i = 0; i < 4; ++i)
            weights[i] = weights[i] * (1.0f - w) + ((i == rule.MaterialSlot) ? w : 0.0f);
    }
    return weights;
}

TerrainSurfaceRule LatitudeRow(float32 min, float32 feather)
{
    TerrainSurfaceRule rule{};
    rule.MaterialSlot = kDefaultSnowSlot;
    rule.ConditionCount = 1;
    rule.Conditions[0].Kind = TerrainRuleConditionKind::LatitudeDegrees;
    rule.Conditions[0].Min = min;
    rule.Conditions[0].Max = 90.0f;
    rule.Conditions[0].Feather = feather;
    return rule;
}

float32 RowWeightAt(const TerrainSurfaceRule& rule, float32 latitude, float32 longitude)
{
    const Vector3 dir = DirectionAt(latitude, longitude);
    const WorldPosition position = PointAt(dir, kPlanetRadius);
    const TerrainRuleSample sample = MakePlanetRuleSample(position, dir, kPlanetRadius, kEveryMeasurement);
    return EvaluateTerrainSurfaceRuleWeight(rule, sample, [](float32, float32, float32, uint32) { return 0.0f; });
}

// The default rows' oracle: weights grass, rock, dirt, snow from the slope against the radial
// up (1 - dot(normal, dir)) and the altitude over the relief envelope.
std::array<float32, 4> SlopeAltitudeBlend(const Vector3& normal, const Vector3& dir, float32 altitude)
{
    constexpr float32 kRockSlopeLow = 0.30f;
    constexpr float32 kRockSlopeHigh = 0.65f;
    constexpr float32 kSnowAltitudeLow = 0.45f;
    constexpr float32 kSnowAltitudeHigh = 0.80f;
    constexpr float32 kDirtAltitudeLow = -0.10f;
    constexpr float32 kDirtAltitudeHigh = 0.40f;
    const float32 slope = std::clamp(1.0f - Vector3::Dot(normal, dir), 0.0f, 1.0f);
    const float32 band = std::clamp(altitude / (CBTTerrain::kReliefEnvelope * kAmplitude), -1.0f, 1.0f);
    const float32 rock = Math::SmoothStep(kRockSlopeLow, kRockSlopeHigh, slope);
    const float32 snow = (1.0f - rock) * Math::SmoothStep(kSnowAltitudeLow, kSnowAltitudeHigh, band);
    const float32 belowSnow = 1.0f - rock - snow;
    const float32 dirt = belowSnow * Math::SmoothStep(kDirtAltitudeLow, kDirtAltitudeHigh, band);
    return {belowSnow - dirt, rock, dirt, snow};
}

// The largest per-channel gap between the rows and the planet blend over a sweep of
// slope, direction (face centers, edges and corners) and height.
float32 MaxGapToTheBlend(const TerrainSurfaceRulesEffect& fx)
{
    const float32 envelope = CBTTerrain::kReliefEnvelope * kAmplitude;
    const std::array<Vector3, 5> dirs = {Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(1, 1, 0).Normalize(),
                                         Vector3(1, 1, 1).Normalize(), Vector3(-0.3f, 0.2f, -0.9f).Normalize()};
    float32 maxGap = 0.0f;
    for (const Vector3& dir : dirs)
        for (float32 tilt = 0.0f; tilt <= 90.0f; tilt += 2.5f)
            for (float32 h = -1.1f * envelope; h <= 1.1f * envelope; h += envelope / 40.0f)
            {
                const Vector3 normal = TiltedNormal(dir, tilt);
                const WorldPosition position = PointAt(dir, kPlanetRadius + h);
                const TerrainRuleSample sample =
                    MakePlanetRuleSample(position, normal, kPlanetRadius, kEveryMeasurement);
                const std::array<float32, 4> rows = ComposePlanetRows(fx, position, sample);
                const std::array<float32, 4> blend = SlopeAltitudeBlend(normal, dir, h);
                for (uint32 i = 0; i < 4; ++i)
                    maxGap = std::max(maxGap, std::abs(rows[i] - blend[i]));
            }
    return maxGap;
}

} // namespace

TEST(TerrainPlanetSurfaceRules, ALatitudeRowPaintsThePolarBandAndNothingElse)
{
    const TerrainSurfaceRule polar = LatitudeRow(70.0f, 8.0f);
    for (float32 latitude = -90.0f; latitude <= 90.0f; latitude += 0.5f)
        for (float32 longitude = 0.0f; longitude < 360.0f; longitude += 45.0f)
        {
            const float32 w = RowWeightAt(polar, latitude, longitude);
            if (std::abs(latitude) >= 70.01f)
            {
                EXPECT_FLOAT_EQ(w, 1.0f) << "lat " << latitude << " lon " << longitude;
            }
            else if (std::abs(latitude) <= 61.99f)
            {
                EXPECT_EQ(w, 0.0f) << "lat " << latitude << " lon " << longitude;
            }
        }
}

TEST(TerrainPlanetSurfaceRules, TheBandEdgeFallsOffOverTheFeather)
{
    const TerrainSurfaceRule polar = LatitudeRow(70.0f, 8.0f);
    EXPECT_NEAR(RowWeightAt(polar, 66.0f, 30.0f), 0.5f, 1e-3f);
    EXPECT_NEAR(RowWeightAt(polar, -64.0f, 200.0f), 0.25f, 1e-3f);
    EXPECT_NEAR(RowWeightAt(polar, 68.0f, 100.0f), 0.75f, 1e-3f);
}

TEST(TerrainPlanetSurfaceRules, LatitudeIsNinetyAtThePolesAndZeroAtTheEquator)
{
    const auto latitudeAt = [](const Vector3& dir) {
        return MakePlanetRuleSample(PointAt(dir, kPlanetRadius), dir, kPlanetRadius, kEveryMeasurement).LatitudeDegrees;
    };
    EXPECT_NEAR(latitudeAt(Vector3(0, 1, 0)), 90.0f, 1e-3f);
    EXPECT_NEAR(latitudeAt(Vector3(0, -1, 0)), 90.0f, 1e-3f);
    EXPECT_NEAR(latitudeAt(Vector3(1, 0, 0)), 0.0f, 1e-4f);
    EXPECT_NEAR(latitudeAt(Vector3(0, 0, -1)), 0.0f, 1e-4f);
    EXPECT_NEAR(latitudeAt(DirectionAt(-35.0f, 123.0f)), 35.0f, 1e-3f);
}

TEST(TerrainPlanetSurfaceRules, SlopeIsMeasuredFromTheLocalUpAtAFaceCenterAndACubeCorner)
{
    for (const Vector3& dir : {Vector3(0, 0, 1), Vector3(1, 1, 1).Normalize(), Vector3(-1, 1, -1).Normalize()})
    {
        const TerrainRuleSample sample =
            MakePlanetRuleSample(PointAt(dir, kPlanetRadius), TiltedNormal(dir, 45.0f), kPlanetRadius,
                                 kEveryMeasurement);
        EXPECT_NEAR(sample.SlopeDegrees, 45.0f, 1e-2f);
        EXPECT_NEAR(sample.SlopeNormalized, 1.0f - std::cos(45.0f * kDegreesToRadians), 1e-5f);
    }
}

TEST(TerrainPlanetSurfaceRules, HeightIsMetersAboveTheReferenceRadius)
{
    const Vector3 dir = Vector3(0.2f, -0.7f, 0.4f).Normalize();
    const TerrainRuleSample above =
        MakePlanetRuleSample(PointAt(dir, kPlanetRadius + 1234.5), dir, kPlanetRadius, kEveryMeasurement);
    EXPECT_NEAR(above.HeightMeters, 1234.5f, 1e-2f);
    const TerrainRuleSample deep =
        MakePlanetRuleSample(PointAt(dir, kPlanetRadius - 9000.0), dir, kPlanetRadius, kEveryMeasurement);
    EXPECT_NEAR(deep.HeightMeters, -9000.0f, 1e-2f);

    const TerrainHeightRangeMeters range = TestPlanetRange();
    EXPECT_EQ(range.Min, -CBTTerrain::kReliefEnvelope * kAmplitude);
    EXPECT_EQ(range.Max, CBTTerrain::kReliefEnvelope * kAmplitude);
}

TEST(TerrainPlanetSurfaceRules, TheDefaultPlanetRowsEqualTheSlopeAltitudeBlend)
{
    const TerrainHeightRangeMeters range = TestPlanetRange();
    EXPECT_LE(MaxGapToTheBlend(MakeDefaultPlanetSurfaceRules(range.Min, range.Max)), 1.0f / 255.0f);
}

TEST(TerrainPlanetSurfaceRules, ThePlanarRowOrderDoesNotEqualTheBlend)
{
    // grass, dirt, rock, snow: snow lands over rock, so cliffs on high ground read as snow.
    const TerrainHeightRangeMeters range = TestPlanetRange();
    TerrainSurfaceRulesEffect planarOrder = MakeDefaultPlanetSurfaceRules(range.Min, range.Max);
    std::swap(planarOrder.Rules[2], planarOrder.Rules[3]);
    EXPECT_GT(MaxGapToTheBlend(planarOrder), 1.0f / 255.0f);
}

TEST(TerrainPlanetSurfaceRules, ThePlanarSampleAndDefaultRowsAreUnchanged)
{
    const TerrainRuleSample sample = MakeTerrainRuleSample(0.0f, 1.0f, 0.0f, 0.5f, 100.0f, 12.0f, -7.0f);
    EXPECT_EQ(sample.SlopeDegrees, 0.0f);
    EXPECT_EQ(sample.HeightMeters, 50.0f);
    EXPECT_EQ(sample.LatitudeDegrees, 0.0f);

    const TerrainSurfaceRulesEffect planar = MakeDefaultTerrainSurfaceRules(0.0f, 100.0f);
    ASSERT_EQ(planar.RuleCount, 4u);
    const std::array<uint32, 4> slots = {kDefaultGrassSlot, kDefaultDirtSlot, kDefaultRockSlot, kDefaultSnowSlot};
    for (uint32 r = 0; r < 4; ++r)
    {
        EXPECT_EQ(planar.Rules[r].MaterialSlot, slots[r]);
        EXPECT_FALSE(planar.Rules[r].Replace);
    }
    const auto noNoise = [](float32, float32, float32, uint32) { return 0.0f; };
    EXPECT_EQ(EvaluateTerrainSurfaceRuleWeight(planar.Rules[0], sample, noNoise), 1.0f);
    EXPECT_EQ(EvaluateTerrainSurfaceRuleWeight(planar.Rules[2], sample, noNoise), 0.0f);
}

TEST(TerrainPlanetSurfaceRules, ThePresetsAreListedByNameAndDomain)
{
    struct Expected
    {
        std::string_view Name;
        TerrainDomain Domain;
    };
    const std::array<Expected, 5> expected = {{{"Temperate", TerrainDomain::Planar},
                                               {"Temperate", TerrainDomain::Spherical},
                                               {"Desert", TerrainDomain::Spherical},
                                               {"Ice Moon", TerrainDomain::Spherical},
                                               {"Barren", TerrainDomain::Spherical}}};
    const auto presets = TerrainSurfaceRulePresets();
    ASSERT_EQ(presets.size(), expected.size());
    for (size_t i = 0; i < presets.size(); ++i)
    {
        EXPECT_EQ(presets[i].Name, expected[i].Name);
        EXPECT_EQ(presets[i].Domain, expected[i].Domain);
        EXPECT_FALSE(presets[i].Description.empty());
        ASSERT_NE(presets[i].Make, nullptr);
        const TerrainSurfaceRulesEffect fx = presets[i].Make(-1000.0f, 1000.0f);
        EXPECT_GE(fx.RuleCount, 1u);
        EXPECT_LE(fx.RuleCount, kMaxTerrainSurfaceRules);
        if (presets[i].Domain != TerrainDomain::Spherical)
            continue;
        // The planet help of raw slope tells a user to author slope in degrees.
        for (uint32 ruleIndex = 0; ruleIndex < fx.RuleCount; ++ruleIndex)
            for (uint32 conditionIndex = 0; conditionIndex < fx.Rules[ruleIndex].ConditionCount; ++conditionIndex)
                EXPECT_NE(fx.Rules[ruleIndex].Conditions[conditionIndex].Kind, TerrainRuleConditionKind::SlopeNormalized)
                    << presets[i].Name << " rule " << ruleIndex;
    }
}

TEST(TerrainPlanetSurfaceRules, TheTemperatePresetAppliesItsRowsInOrder)
{
    const TerrainHeightRangeMeters range = TestPlanetRange();
    const TerrainSurfaceRulesEffect fx = TerrainSurfaceRulePresets()[1].Make(range.Min, range.Max);
    ASSERT_EQ(fx.RuleCount, 5u);
    EXPECT_EQ(fx.Rules[3].Conditions[0].Kind, TerrainRuleConditionKind::LatitudeDegrees);
    EXPECT_EQ(fx.Rules[4].Conditions[0].Kind, TerrainRuleConditionKind::SlopeDegrees)
        << "the Rock row's own help asks for slope in degrees";

    // Flat ground at a pole: the polar row lands over grass.
    const Vector3 pole(0, 1, 0);
    const WorldPosition polePoint = PointAt(pole, kPlanetRadius);
    const std::array<float32, 4> flatPole =
        ComposePlanetRows(fx, polePoint, MakePlanetRuleSample(polePoint, pole, kPlanetRadius, kEveryMeasurement));
    EXPECT_NEAR(flatPole[kDefaultSnowSlot], 1.0f, 1e-5f);

    // A cliff at the same pole: rock is the last row, so it lands over the polar snow.
    const std::array<float32, 4> cliffPole = ComposePlanetRows(
        fx, polePoint, MakePlanetRuleSample(polePoint, TiltedNormal(pole, 80.0f), kPlanetRadius, kEveryMeasurement));
    EXPECT_NEAR(cliffPole[kDefaultRockSlot], 1.0f, 1e-5f);

    // Flat ground at the equator and the datum: mostly grass, no snow and no rock.
    const Vector3 equator(1, 0, 0);
    const WorldPosition equatorPoint = PointAt(equator, kPlanetRadius);
    const std::array<float32, 4> flatEquator =
        ComposePlanetRows(fx, equatorPoint,
                          MakePlanetRuleSample(equatorPoint, equator, kPlanetRadius, kEveryMeasurement));
    EXPECT_GT(flatEquator[kDefaultGrassSlot], 0.5f);
    EXPECT_EQ(flatEquator[kDefaultSnowSlot], 0.0f);
    EXPECT_EQ(flatEquator[kDefaultRockSlot], 0.0f);
}

namespace
{

// The composed weights of a planet preset at a direction, a height above the radius and a slope.
std::array<float32, 4> PresetAt(std::string_view name, const Vector3& dir, float64 heightMeters, float32 tiltDegrees)
{
    const TerrainHeightRangeMeters range = TestPlanetRange();
    for (const TerrainSurfaceRulePreset& preset : TerrainSurfaceRulePresets())
    {
        if (preset.Domain != TerrainDomain::Spherical || preset.Name != name)
            continue;
        const TerrainSurfaceRulesEffect fx = preset.Make(range.Min, range.Max);
        const WorldPosition point = PointAt(dir, kPlanetRadius + heightMeters);
        return ComposePlanetRows(
            fx, point, MakePlanetRuleSample(point, TiltedNormal(dir, tiltDegrees), kPlanetRadius, kEveryMeasurement));
    }
    ADD_FAILURE() << "no planet preset named " << name;
    return {};
}

} // namespace

TEST(TerrainPlanetSurfaceRules, TheDesertPresetIsDirtWithRockSummitsAndNoSnow)
{
    const Vector3 dir = DirectionAt(20.0f, 40.0f);
    EXPECT_NEAR(PresetAt("Desert", dir, 0.0, 0.0f)[kDefaultDirtSlot], 1.0f, 1e-5f);
    EXPECT_NEAR(PresetAt("Desert", dir, 2900.0, 0.0f)[kDefaultRockSlot], 1.0f, 1e-5f);
    EXPECT_NEAR(PresetAt("Desert", dir, 0.0, 80.0f)[kDefaultRockSlot], 1.0f, 1e-5f);
    for (const Vector3& pose : {DirectionAt(85.0f, 0.0f), dir})
        for (const float64 height : {-2900.0, 0.0, 2900.0})
            EXPECT_EQ(PresetAt("Desert", pose, height, 0.0f)[kDefaultSnowSlot], 0.0f) << height;
}

TEST(TerrainPlanetSurfaceRules, TheIceMoonPresetIsSnowWithRockCliffs)
{
    const Vector3 dir = DirectionAt(-10.0f, 200.0f);
    EXPECT_NEAR(PresetAt("Ice Moon", dir, 0.0, 0.0f)[kDefaultSnowSlot], 1.0f, 1e-5f);
    EXPECT_NEAR(PresetAt("Ice Moon", dir, 0.0, 80.0f)[kDefaultRockSlot], 1.0f, 1e-5f);
}

TEST(TerrainPlanetSurfaceRules, TheBarrenPresetHasDustOnlyOnGentleLowlands)
{
    const Vector3 dir = DirectionAt(35.0f, 300.0f);
    EXPECT_NEAR(PresetAt("Barren", dir, -2000.0, 0.0f)[kDefaultDirtSlot], 1.0f, 1e-5f);
    EXPECT_NEAR(PresetAt("Barren", dir, -2000.0, 80.0f)[kDefaultRockSlot], 1.0f, 1e-5f) << "a steep lowland stays rock";
    EXPECT_NEAR(PresetAt("Barren", dir, 1000.0, 0.0f)[kDefaultRockSlot], 1.0f, 1e-5f);
}

TEST(TerrainPlanetSurfaceRules, ThePlanetNoiseIsOneValueAtAPointInTheUnitRange)
{
    const WorldPosition point = PointAt(Vector3(1, 1, 1).Normalize(), kPlanetRadius);
    const float32 value = PlanetRuleNoiseSample(point, 0.02f, 7u);
    EXPECT_EQ(value, PlanetRuleNoiseSample(point, 0.02f, 7u));
    EXPECT_NE(value, PlanetRuleNoiseSample(point, 0.02f, 8u));
    for (float32 x = 0.0f; x < 500.0f; x += 7.3f)
    {
        const float32 v = PlanetRuleNoiseSample(WorldPosition{kPlanetRadius + x, x * 0.5, -x}, 0.02f, 3u);
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
}
