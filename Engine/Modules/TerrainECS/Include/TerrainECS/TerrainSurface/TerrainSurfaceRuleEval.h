#pragma once

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Transcendental.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/WorldPosition.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace GameEngine::TerrainECS
{

// ---- Surface rule evaluation ------------------------------------------------
//
// Pure maths over one authored rule row: no terrain, no modifier, no splat. The
// bake supplies a texel's measurements and gets back a weight; what that weight
// then does to the splat belongs to the bake.
//
// Split out of the bake TU for two reasons. It is the part worth testing on its
// own — band edges, curve shapes, unit conversions and the AND product are all
// hand-checkable — and it is the part the GPU splat kernel has to reproduce
// expression for expression when rules reach it, which is far easier against a
// function than against a loop nested in a texel walk.

// A texel's measurements, computed once per texel by the bake and reused by
// every rule row and condition that reads them.
inline constexpr float32 kDegreesPerRadian = 180.0f / std::numbers::pi_v<float32>;

struct TerrainRuleSample
{
    // `1 - max(N.y, 0)` off the heightfield's own normal — 0 flat, 1 steep. NOT a
    // geometric cosine: the heightfield stores height normalized to [0, 1] while the
    // sample spacing is in metres, so this quantity is squashed by the terrain's
    // HeightScale and a threshold in it means a different ANGLE on every terrain.
    // Authored rows should band SlopeDegrees below; this is kept only because the
    // condition kind is shipped and a scene may already carry it.
    float32 SlopeNormalized = 0.0f;
    // The true surface angle from horizontal, in degrees over [0, 90] — measured
    // from the gradient with height scaled back into metres. Independently
    // computed rather than derived from SlopeNormalized, because the conversion
    // between them is the terrain's HeightScale and a rule row does not carry it.
    float32 SlopeDegrees = 0.0f;
    // Meters above the terrain's base: the normalized height sample times
    // HeightScale. NOT world-space Y: the terrain entity's own Y translation is
    // not folded in, matching the other modifier effects.
    float32 HeightMeters = 0.0f;
    // World XZ, for the noise field.
    float32 WorldX = 0.0f;
    float32 WorldZ = 0.0f;
    // Absolute latitude in degrees over [0, 90]. A flat terrain has none and reads 0.
    float32 LatitudeDegrees = 0.0f;
};

// Build a texel's measurements from what the bake already has in hand: the
// heightfield normal at the texel, the normalized height sample, and the two
// scales that turn normalized quantities into authorable ones.
//
// A function rather than nine assignments in the texel walk, because the unit
// conversions are the part most likely to drift from the GPU mirror and the
// mirror is far easier to hold against a function. It stays free of terrain and
// modifier types for the same reason the rest of this header does — everything
// it needs is a float.
inline TerrainRuleSample MakeTerrainRuleSample(float32 normalX, float32 normalY, float32 normalZ,
                                               float32 sampleHeight, float32 heightScale,
                                               float32 worldX, float32 worldZ)
{
    TerrainRuleSample sample{};
    sample.SlopeNormalized = 1.0f - std::max(normalY, 0.0f);
    // The gradient the normal came from, with height taken back into metres —
    // the angle a person means by "34 degrees". normalY is strictly positive (it
    // is 1 before normalizing).
    const float32 invY = 1.0f / normalY;
    const float32 gradX = -normalX * invY * heightScale;
    const float32 gradZ = -normalZ * invY * heightScale;
    sample.SlopeDegrees =
        Math::Atan(std::sqrt(gradX * gradX + gradZ * gradZ)) * kDegreesPerRadian;
    // Terrain-local meters: the normalized sample scaled back up, with no terrain
    // entity translation folded in.
    sample.HeightMeters = sampleHeight * heightScale;
    sample.WorldX = worldX;
    sample.WorldZ = worldZ;
    return sample;
}

// The measurements that a set of rule rows reads. A sample need not compute the others.
struct TerrainRuleMeasurements
{
    bool SlopeNormalized = true;
    bool SlopeDegrees = true;
    bool HeightMeters = true;
    bool LatitudeDegrees = true;
    // The rows sample the noise field at the texel's position.
    bool Noise = true;
};

// The measurements that the conditions of `rules` read: each condition up to its rule's clamped count.
inline TerrainRuleMeasurements TerrainRuleMeasurementsOf(std::span<const Components::TerrainSurfaceRule> rules)
{
    using Kind = Components::TerrainRuleConditionKind;
    TerrainRuleMeasurements read{false, false, false, false, false};
    for (const Components::TerrainSurfaceRule& rule : rules)
    {
        const uint32 conditionCount = std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);
        for (uint32 i = 0; i < conditionCount; ++i)
        {
            const Kind kind = rule.Conditions[i].Kind;
            read.SlopeNormalized = read.SlopeNormalized || kind == Kind::SlopeNormalized;
            read.SlopeDegrees = read.SlopeDegrees || kind == Kind::SlopeDegrees;
            read.HeightMeters = read.HeightMeters || kind == Kind::HeightMeters;
            read.LatitudeDegrees = read.LatitudeDegrees || kind == Kind::LatitudeDegrees;
            read.Noise = read.Noise || kind == Kind::Noise;
        }
    }
    return read;
}

// A planet texel's measurements against the local up `dir = position / |position|`.
// `position` is planet-local, in meters; `normal` is the unit surface normal. Height is
// meters above the reference radius. Latitude is about the planet-local +Y axis.
// A measurement that `measured` leaves out stays 0; each other one has the same bits as with all.
inline TerrainRuleSample MakePlanetRuleSample(const Mathematics::WorldPosition& position,
                                              const Mathematics::Vector3& normal, float64 referenceRadius,
                                              const TerrainRuleMeasurements& measured)
{
    const float64 horizontal = std::sqrt(position.X * position.X + position.Z * position.Z);
    const float64 radius = std::sqrt(horizontal * horizontal + position.Y * position.Y);
    TerrainRuleSample sample{};
    if (!(radius > 0.0))
        return sample;

    if (measured.SlopeNormalized || measured.SlopeDegrees)
    {
        const float32 upX = static_cast<float32>(position.X / radius);
        const float32 upY = static_cast<float32>(position.Y / radius);
        const float32 upZ = static_cast<float32>(position.Z / radius);
        const float32 cosine = std::clamp(normal.x * upX + normal.y * upY + normal.z * upZ, -1.0f, 1.0f);
        if (measured.SlopeNormalized)
            sample.SlopeNormalized = Math::Clamp01(1.0f - cosine);
        if (measured.SlopeDegrees)
            sample.SlopeDegrees = std::acos(std::max(cosine, 0.0f)) * kDegreesPerRadian;
    }
    if (measured.HeightMeters)
        sample.HeightMeters = static_cast<float32>(radius - referenceRadius);
    // atan2 in float64 keeps full resolution near the poles, where asin of |up.y| does not.
    if (measured.LatitudeDegrees)
        sample.LatitudeDegrees =
            static_cast<float32>(std::atan2(std::abs(position.Y), horizontal)) * kDegreesPerRadian;
    return sample;
}

// The condition's own measurement, in the unit its kind names. Noise is absent
// here because it is the one kind that needs a field sampled at (X, Z) rather
// than a value the texel already carries.
inline float32 TerrainRuleConditionValue(Components::TerrainRuleConditionKind kind,
                                         const TerrainRuleSample& sample)
{
    using Kind = Components::TerrainRuleConditionKind;
    switch (kind)
    {
    case Kind::SlopeNormalized:
        return sample.SlopeNormalized;
    case Kind::SlopeDegrees:
        return sample.SlopeDegrees;
    case Kind::HeightMeters:
        return sample.HeightMeters;
    case Kind::LatitudeDegrees:
        return sample.LatitudeDegrees;
    case Kind::Noise:
        break;
    }
    return 0.0f;
}

// Ramp shape across an already-clamped [0, 1] edge parameter.
inline float32 TerrainRuleCurve(Components::TerrainRuleFalloffCurve curve, float32 t)
{
    if (curve == Components::TerrainRuleFalloffCurve::Smoothstep)
        return t * t * (3.0f - 2.0f * t);
    return t;
}

// Weight [0, 1] for one condition given its already-measured value.
//
// 1 inside [Min, Max]; outside, a ramp of width Feather down to 0. Feather 0 is
// a hard edge. A degenerate band (Min == Max) is therefore a symmetric peak, and
// an inverted band (Min > Max) has no plateau — only the two ramps, which is the
// honest reading of an empty band rather than a silently corrected one.
inline float32 TerrainRuleConditionWeightForValue(const Components::TerrainRuleCondition& condition,
                                                  float32 value)
{
    if (value >= condition.Min && value <= condition.Max)
        return 1.0f;
    if (condition.Feather <= 0.0f)
        return 0.0f;

    const float32 distance = (value < condition.Min) ? (condition.Min - value)
                                                     : (value - condition.Max);
    const float32 t = Math::Clamp01(1.0f - distance / condition.Feather);
    return TerrainRuleCurve(condition.FalloffCurve, t);
}

// Weight [0, 1] for one rule row: its conditions ANDed by multiplication, times
// the row's strength. A row with no conditions is unconditional.
//
// `noiseSampler` is invoked as `noise(worldX, worldZ, frequency, seed)` and only
// for Kind::Noise. It is a parameter rather than a call into the terrain noise
// basis so this header stays free of it — and so a test can drive a rule with a
// field whose value at a point is known by construction.
template <typename NoiseSampler>
float32 EvaluateTerrainSurfaceRuleWeight(const Components::TerrainSurfaceRule& rule,
                                         const TerrainRuleSample& sample,
                                         NoiseSampler&& noiseSampler)
{
    float32 weight = rule.Strength;
    const uint32 conditionCount =
        std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);

    for (uint32 i = 0; i < conditionCount && weight > 0.0f; ++i)
    {
        const Components::TerrainRuleCondition& condition = rule.Conditions[i];
        const float32 value =
            (condition.Kind == Components::TerrainRuleConditionKind::Noise)
                ? noiseSampler(sample.WorldX, sample.WorldZ, condition.NoiseFrequency,
                               condition.NoiseSeed)
                : TerrainRuleConditionValue(condition.Kind, sample);
        weight *= TerrainRuleConditionWeightForValue(condition, value);
    }

    return Math::Clamp01(weight);
}

} // namespace GameEngine::TerrainECS
