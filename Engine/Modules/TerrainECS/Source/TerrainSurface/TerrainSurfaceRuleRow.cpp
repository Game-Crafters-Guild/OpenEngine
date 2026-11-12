#include "TerrainSurface/TerrainSurfaceRuleRow.h"

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Mathematics/Float8.h"
#include "Mathematics/WorldPosition.h"
#include "TerrainECS/TerrainRuleNoise.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>

namespace GameEngine::TerrainECS
{
namespace
{

using Mathematics::Float8;
constexpr std::size_t kLanes = Float8::kLaneCount;

// Lanes `first` to `first + count` of `plane`; the lanes past `count` are 0.
Float8 GE_VECTORCALL LoadLanes(std::span<const float32> plane, std::size_t first, std::size_t count)
{
    if (count == kLanes)
        return Float8::Load(plane.subspan(first).first<kLanes>());
    alignas(32) std::array<float32, kLanes> lanes{};
    std::copy_n(plane.begin() + static_cast<std::ptrdiff_t>(first), count, lanes.begin());
    return Float8::Load(lanes);
}

// Writes the first `count` lanes of `values` to `plane` from `first`.
void GE_VECTORCALL StoreLanes(Float8 values, std::span<float32> plane, std::size_t first, std::size_t count)
{
    if (count == kLanes)
    {
        values.Store(plane.subspan(first).first<kLanes>());
        return;
    }
    alignas(32) std::array<float32, kLanes> lanes{};
    values.Store(lanes);
    std::copy_n(lanes.begin(), count, plane.begin() + static_cast<std::ptrdiff_t>(first));
}

// The plane of a condition kind that has a field, or an empty span.
std::span<const float32> PlaneOf(const TerrainRuleSampleRow& samples, Components::TerrainRuleConditionKind kind)
{
    using Kind = Components::TerrainRuleConditionKind;
    switch (kind)
    {
    case Kind::SlopeNormalized:
        return samples.SlopeNormalized;
    case Kind::SlopeDegrees:
        return samples.SlopeDegrees;
    case Kind::HeightMeters:
        return samples.HeightMeters;
    case Kind::LatitudeDegrees:
        return samples.LatitudeDegrees;
    case Kind::Noise:
        break;
    }
    return {};
}

// True when each plane that a condition of `rule` reads, and the positions for Noise, are `length` long.
// Only an assert calls it, so a build without asserts does not.
[[maybe_unused]] bool ReadPlanesFitTheRow(const Components::TerrainSurfaceRule& rule,
                                          const TerrainRuleSampleRow& samples,
                                          std::span<const Mathematics::WorldPosition> positions, std::size_t length)
{
    using Kind = Components::TerrainRuleConditionKind;
    const uint32 conditionCount = std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);
    for (uint32 i = 0; i < conditionCount; ++i)
    {
        const Kind kind = rule.Conditions[i].Kind;
        const bool hasField = kind == Kind::SlopeNormalized || kind == Kind::SlopeDegrees ||
                              kind == Kind::HeightMeters || kind == Kind::LatitudeDegrees;
        if (kind == Kind::Noise && positions.size() != length)
            return false;
        if (hasField && PlaneOf(samples, kind).size() != length)
            return false;
    }
    return true;
}

// The condition's measurement in lanes: PlanetRuleNoiseSample per lane for Noise, the plane of the kind,
// or 0 for a kind without a field, as TerrainRuleConditionValue gives.
Float8 GE_VECTORCALL ConditionValues(const Components::TerrainRuleCondition& condition,
                                     const TerrainRuleSampleRow& samples,
                                     std::span<const Mathematics::WorldPosition> positions, std::size_t first,
                                     std::size_t count)
{
    if (condition.Kind == Components::TerrainRuleConditionKind::Noise)
    {
        alignas(32) std::array<float32, kLanes> noise{};
        for (std::size_t lane = 0; lane < count; ++lane)
            noise[lane] = PlanetRuleNoiseSample(positions[first + lane], condition.NoiseFrequency, condition.NoiseSeed);
        return Float8::Load(noise);
    }
    const std::span<const float32> plane = PlaneOf(samples, condition.Kind);
    if (plane.empty())
        return Float8::Broadcast(0.0f);
    return LoadLanes(plane, first, count);
}

// TerrainRuleCurve in each lane, with its bits.
Float8 GE_VECTORCALL RuleCurve(Components::TerrainRuleFalloffCurve curve, Float8 t)
{
    if (curve == Components::TerrainRuleFalloffCurve::Smoothstep)
        return t * t * (Float8::Broadcast(3.0f) - Float8::Broadcast(2.0f) * t);
    return t;
}

// TerrainRuleConditionWeightForValue in each lane, with its bits. The nested Select is the scalar
// `value >= Min && value <= Max`; a NaN value fails both and takes the ramp, as in the scalar.
Float8 GE_VECTORCALL ConditionWeight(const Components::TerrainRuleCondition& condition, Float8 value)
{
    const Float8 min = Float8::Broadcast(condition.Min);
    const Float8 max = Float8::Broadcast(condition.Max);
    const Float8 one = Float8::Broadcast(1.0f);
    Float8 outside = Float8::Broadcast(0.0f);
    // The scalar returns 0 only when `Feather <= 0`: a NaN feather takes the ramp.
    if (!(condition.Feather <= 0.0f))
    {
        const Float8 distance = Float8::Select(value < min, min - value, value - max);
        const Float8 t = Float8::Clamp01(one - distance / Float8::Broadcast(condition.Feather));
        outside = RuleCurve(condition.FalloffCurve, t);
    }
    return Float8::Select(value >= min, Float8::Select(value <= max, one, outside), outside);
}

// The rule's weight in lanes `first` to `first + count`. A lane stops at the first condition where its
// weight is not above 0, as the scalar loop does; the block stops when no lane goes on.
Float8 GE_VECTORCALL RuleWeightLanes(const Components::TerrainSurfaceRule& rule, const TerrainRuleSampleRow& samples,
                                     std::span<const Mathematics::WorldPosition> positions, std::size_t first,
                                     std::size_t count)
{
    const Float8 zero = Float8::Broadcast(0.0f);
    Float8 weight = Float8::Broadcast(rule.Strength);
    const uint32 conditionCount = std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);
    for (uint32 i = 0; i < conditionCount; ++i)
    {
        const Float8::Mask active = weight > zero;
        if (!active.Any())
            break;
        const Components::TerrainRuleCondition& condition = rule.Conditions[i];
        const Float8 value = ConditionValues(condition, samples, positions, first, count);
        weight = Float8::Select(active, weight * ConditionWeight(condition, value), weight);
    }
    return Float8::Clamp01(weight);
}

} // namespace

void EvaluatePlanetSurfaceRuleWeightRow(const Components::TerrainSurfaceRule& rule, const TerrainRuleSampleRow& samples,
                                        std::span<const Mathematics::WorldPosition> positions,
                                        std::span<float32> weights)
{
    const std::size_t length = weights.size();
    assert(ReadPlanesFitTheRow(rule, samples, positions, length) &&
           "EvaluatePlanetSurfaceRuleWeightRow: a plane or the positions that the rule reads are not as long as the row");
    for (std::size_t first = 0; first < length; first += kLanes)
    {
        const std::size_t count = std::min(kLanes, length - first);
        StoreLanes(RuleWeightLanes(rule, samples, positions, first, count), weights, first, count);
    }
}

} // namespace GameEngine::TerrainECS
