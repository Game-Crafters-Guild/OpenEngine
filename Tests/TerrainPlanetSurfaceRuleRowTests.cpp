// The planet's rule weights over a row of texels against the scalar weight, bit for bit: each condition
// kind, rows of 0, 7, 8, 13 and 136 texels, on six faces and across their edges, with a NaN, a negative
// zero, a denormal and a value past the domain among the measurements.

#include <gtest/gtest.h>

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Mathematics/CubeFaces.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "TerrainECS/TerrainRuleNoise.h"
#include "TerrainECS/TerrainSurface/TerrainSurfaceRuleEval.h"
#include "TerrainSurface/TerrainSurfaceRuleRow.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::TerrainECS;
using Mathematics::Vector3;
using Mathematics::WorldPosition;

namespace
{

constexpr float64 kRadius = 600000.0;
constexpr std::size_t kRowTexels = 136;
// 13 is a full block of eight, then a short last block.
constexpr std::array<std::size_t, 5> kRowLengths = {0, 7, 8, 13, kRowTexels};
constexpr float32 kDegreesToRadians = std::numbers::pi_v<float32> / 180.0f;

Vector3 AxisVector(uint8 axis, float32 value)
{
    return Vector3(axis == 0 ? value : 0.0f, axis == 1 ? value : 0.0f, axis == 2 ? value : 0.0f);
}

// One row of texels: the positions, the samples with every measurement, and the planes made from them.
struct RuleRow
{
    std::vector<WorldPosition> Positions;
    std::vector<Vector3> Normals;
    std::vector<TerrainRuleSample> Samples;
    std::vector<float32> SlopeNormalized, SlopeDegrees, HeightMeters, LatitudeDegrees;

    TerrainRuleSampleRow Planes(std::size_t length) const
    {
        return {std::span(SlopeNormalized).first(length), std::span(SlopeDegrees).first(length),
                std::span(HeightMeters).first(length), std::span(LatitudeDegrees).first(length)};
    }
};

// A row on `face` at face coordinate `v`, with u from -1.08 to 1.08: its ends are past both face edges,
// on the neighbor faces. The heights span -150 to 150 m and the normals tilt 0 to 90 degrees.
RuleRow MakeRow(uint8 face, float32 v)
{
    const Mathematics::CubeFaceAxes& axes = Mathematics::kCubeFaceAxes[face];
    RuleRow row;
    for (std::size_t i = 0; i < kRowTexels; ++i)
    {
        const float32 u = -1.08f + 2.16f * static_cast<float32>(i) / static_cast<float32>(kRowTexels - 1);
        const Vector3 direction = (AxisVector(axes.NormalAxis, static_cast<float32>(axes.NormalSign)) +
                                   AxisVector(axes.XAxis, u * static_cast<float32>(axes.XSign)) +
                                   AxisVector(axes.ZAxis, v * static_cast<float32>(axes.ZSign)))
                                      .Normalize();
        const float64 radius = kRadius + 150.0 * std::sin(0.37 * static_cast<float64>(i) + static_cast<float64>(face));
        const WorldPosition position{direction.x * radius, direction.y * radius, direction.z * radius};
        const Vector3 helper = std::abs(direction.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
        const Vector3 tangent = Vector3::Cross(direction, helper).Normalize();
        const float32 tilt = static_cast<float32>((i * 7u + face * 13u) % 91u) * kDegreesToRadians;
        const Vector3 normal = (direction * std::cos(tilt) + tangent * std::sin(tilt)).Normalize();
        const TerrainRuleSample sample = MakePlanetRuleSample(position, normal, kRadius, TerrainRuleMeasurements{});
        row.Positions.push_back(position);
        row.Normals.push_back(normal);
        row.Samples.push_back(sample);
        row.SlopeNormalized.push_back(sample.SlopeNormalized);
        row.SlopeDegrees.push_back(sample.SlopeDegrees);
        row.HeightMeters.push_back(sample.HeightMeters);
        row.LatitudeDegrees.push_back(sample.LatitudeDegrees);
    }
    return row;
}

// The rows of six faces: one inside each face, and one on the face's edge at v = 1.
std::vector<RuleRow> SixFaceRows()
{
    std::vector<RuleRow> rows;
    for (uint8 face = 0; face < Mathematics::kCubeFaceCount; ++face)
    {
        rows.push_back(MakeRow(face, 0.37f - 0.11f * face));
        rows.push_back(MakeRow(face, 1.0f));
    }
    return rows;
}

// `row` with a NaN, a negative zero, a denormal and `pastTheDomain` in every measurement of four texels, in the
// planes and in the samples: two texels in the first block of eight and two in the short block of a 13 row.
RuleRow WithUnusualMeasurements(RuleRow row, float32 pastTheDomain)
{
    const std::array<std::pair<std::size_t, float32>, 4> texels = {
        {{1, std::numeric_limits<float32>::quiet_NaN()}, {6, -0.0f}, {9, 1e-40f}, {12, pastTheDomain}}};
    for (const auto& [texel, value] : texels)
    {
        row.SlopeNormalized[texel] = row.SlopeDegrees[texel] = row.HeightMeters[texel] = row.LatitudeDegrees[texel] =
            value;
        TerrainRuleSample& sample = row.Samples[texel];
        sample.SlopeNormalized = sample.SlopeDegrees = sample.HeightMeters = sample.LatitudeDegrees = value;
    }
    return row;
}

TerrainRuleCondition Condition(TerrainRuleConditionKind kind, float32 min, float32 max, float32 feather,
                               TerrainRuleFalloffCurve curve)
{
    TerrainRuleCondition condition{};
    condition.Kind = kind;
    condition.FalloffCurve = curve;
    condition.Min = min;
    condition.Max = max;
    condition.Feather = feather;
    condition.NoiseFrequency = 0.013f;
    condition.NoiseSeed = 11u;
    return condition;
}

TerrainSurfaceRule Rule(float32 strength, std::span<const TerrainRuleCondition> conditions)
{
    TerrainSurfaceRule rule{};
    rule.Strength = strength;
    rule.ConditionCount = static_cast<uint8>(conditions.size());
    for (std::size_t i = 0; i < conditions.size(); ++i)
        rule.Conditions[i] = conditions[i];
    return rule;
}

// Rules of `kind` over its domain [low, high]: a feathered band of each curve, a hard edge, an inverted
// band, a peak, a NaN feather, strengths that clamp, and a second condition after the first.
std::vector<TerrainSurfaceRule> RulesOf(TerrainRuleConditionKind kind, float32 low, float32 high)
{
    using Curve = TerrainRuleFalloffCurve;
    const float32 range = high - low;
    const float32 bandMin = low + 0.3f * range, bandMax = low + 0.6f * range, feather = 0.2f * range;
    const TerrainRuleCondition linear = Condition(kind, bandMin, bandMax, feather, Curve::ClampedLinear);
    const TerrainRuleCondition smooth = Condition(kind, bandMin, bandMax, feather, Curve::Smoothstep);
    const TerrainRuleCondition hard = Condition(kind, bandMin, bandMax, 0.0f, Curve::Smoothstep);
    const TerrainRuleCondition inverted = Condition(kind, bandMax, bandMin, feather, Curve::ClampedLinear);
    const TerrainRuleCondition peak = Condition(kind, bandMin, bandMin, feather, Curve::Smoothstep);
    const TerrainRuleCondition nanFeather =
        Condition(kind, bandMin, bandMax, std::numeric_limits<float32>::quiet_NaN(), Curve::ClampedLinear);
    const TerrainRuleCondition upper = Condition(kind, low + 0.45f * range, high, 0.3f * range, Curve::Smoothstep);
    const TerrainRuleCondition height =
        Condition(TerrainRuleConditionKind::HeightMeters, -40.0f, 200.0f, 60.0f, Curve::Smoothstep);
    return {Rule(1.0f, std::array{linear}),
            Rule(1.0f, std::array{smooth}),
            Rule(1.0f, std::array{hard}),
            Rule(1.0f, std::array{inverted}),
            Rule(0.7f, std::array{peak}),
            Rule(1.0f, std::array{nanFeather}),
            Rule(1.8f, std::array{smooth}),
            Rule(-0.5f, std::array{smooth}),
            Rule(1.0f, std::array{smooth, upper}),
            Rule(1.3f, std::array{height, linear, upper}),
            Rule(0.6f, std::span<const TerrainRuleCondition>{})};
}

// Bit equality, with every NaN equal to every NaN: NaN payloads are not part of the contract.
bool SameBits(float32 actual, float32 expected)
{
    if (std::isnan(actual) || std::isnan(expected))
        return std::isnan(actual) && std::isnan(expected);
    return std::bit_cast<uint32>(actual) == std::bit_cast<uint32>(expected);
}

struct RowComparison
{
    uint32 Mismatches = 0;
    uint32 Compared = 0;
    uint32 Zero = 0;
    uint32 One = 0;
    uint32 Between = 0;
};

// Each rule of `kind` over each row of six faces at each row length, against the scalar weight of each texel.
RowComparison CompareRowsWithTheScalarWeight(TerrainRuleConditionKind kind, float32 low, float32 high)
{
    RowComparison result;
    std::vector<RuleRow> rows;
    for (const RuleRow& row : SixFaceRows())
        rows.push_back(WithUnusualMeasurements(row, high + 0.5f * (high - low)));
    for (const TerrainSurfaceRule& rule : RulesOf(kind, low, high))
        for (const RuleRow& row : rows)
            for (const std::size_t length : kRowLengths)
            {
                // A sentinel past the row: the row form writes no weight past its length.
                std::vector<float32> weights(length + 1u, -7.0f);
                EvaluatePlanetSurfaceRuleWeightRow(rule, row.Planes(length), std::span(row.Positions).first(length),
                                                   std::span(weights).first(length));
                result.Mismatches += SameBits(weights[length], -7.0f) ? 0u : 1u;
                for (std::size_t i = 0; i < length; ++i)
                {
                    const WorldPosition& position = row.Positions[i];
                    const float32 expected = EvaluateTerrainSurfaceRuleWeight(
                        rule, row.Samples[i], [&position](float32, float32, float32 frequency, uint32 seed) {
                            return PlanetRuleNoiseSample(position, frequency, seed);
                        });
                    result.Mismatches += SameBits(weights[i], expected) ? 0u : 1u;
                    ++result.Compared;
                    result.Zero += expected == 0.0f ? 1u : 0u;
                    result.One += expected == 1.0f ? 1u : 0u;
                    result.Between += expected > 0.0f && expected < 1.0f ? 1u : 0u;
                }
            }
    return result;
}

void ExpectRowsHaveTheScalarBits(TerrainRuleConditionKind kind, float32 low, float32 high)
{
    const RowComparison result = CompareRowsWithTheScalarWeight(kind, low, high);
    EXPECT_EQ(result.Mismatches, 0u) << "of " << result.Compared;
    // The rows reach the plateau, the ramps and the outside, so a wrong lane operation shows.
    EXPECT_GT(result.Zero, 0u);
    EXPECT_GT(result.One, 0u);
    EXPECT_GT(result.Between, 0u);
}

} // namespace

TEST(PlanetSurfaceRuleWeightRow, ASlopeNormalizedRowHasTheBitsOfTheScalarWeight)
{
    ExpectRowsHaveTheScalarBits(TerrainRuleConditionKind::SlopeNormalized, 0.0f, 1.0f);
}

TEST(PlanetSurfaceRuleWeightRow, ASlopeDegreesRowHasTheBitsOfTheScalarWeight)
{
    ExpectRowsHaveTheScalarBits(TerrainRuleConditionKind::SlopeDegrees, 0.0f, 90.0f);
}

TEST(PlanetSurfaceRuleWeightRow, AHeightRowHasTheBitsOfTheScalarWeight)
{
    ExpectRowsHaveTheScalarBits(TerrainRuleConditionKind::HeightMeters, -150.0f, 150.0f);
}

TEST(PlanetSurfaceRuleWeightRow, ALatitudeRowHasTheBitsOfTheScalarWeight)
{
    ExpectRowsHaveTheScalarBits(TerrainRuleConditionKind::LatitudeDegrees, 0.0f, 90.0f);
}

TEST(PlanetSurfaceRuleWeightRow, ANoiseRowHasTheBitsOfTheScalarWeight)
{
    ExpectRowsHaveTheScalarBits(TerrainRuleConditionKind::Noise, 0.0f, 1.0f);
}

// A sample of some measurements has the bits of the full sample in each of them and 0 in the others, and
// the rules name the measurements their conditions read.
TEST(PlanetSurfaceRuleWeightRow, ASampleOfSomeMeasurementsHasTheirBitsAndZeroForTheRest)
{
    using Kind = TerrainRuleConditionKind;
    const std::array<TerrainRuleCondition, 2> conditions = {
        Condition(Kind::HeightMeters, 0.0f, 1.0f, 1.0f, TerrainRuleFalloffCurve::Smoothstep),
        Condition(Kind::SlopeNormalized, 0.0f, 1.0f, 1.0f, TerrainRuleFalloffCurve::Smoothstep)};
    const std::array<TerrainSurfaceRule, 2> rules = {Rule(1.0f, conditions), Rule(1.0f, {})};
    const TerrainRuleMeasurements measured = TerrainRuleMeasurementsOf(rules);
    EXPECT_TRUE(measured.HeightMeters && measured.SlopeNormalized);
    EXPECT_FALSE(measured.SlopeDegrees || measured.LatitudeDegrees || measured.Noise);

    uint32 mismatches = 0;
    for (const RuleRow& row : SixFaceRows())
        for (std::size_t i = 0; i < kRowTexels; ++i)
        {
            const TerrainRuleSample& all = row.Samples[i];
            const TerrainRuleSample some = MakePlanetRuleSample(row.Positions[i], row.Normals[i], kRadius, measured);
            mismatches += SameBits(some.HeightMeters, all.HeightMeters) ? 0u : 1u;
            mismatches += SameBits(some.SlopeNormalized, all.SlopeNormalized) ? 0u : 1u;
            mismatches += SameBits(some.SlopeDegrees, 0.0f) && SameBits(some.LatitudeDegrees, 0.0f) ? 0u : 1u;
        }
    EXPECT_EQ(mismatches, 0u);
}
