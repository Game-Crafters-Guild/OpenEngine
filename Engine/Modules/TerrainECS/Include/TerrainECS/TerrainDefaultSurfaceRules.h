#pragma once

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

// ---- The default surface rule rows ------------------------------------------
//
// The rows a new terrain's global rules modifier carries. They are an ORDINARY
// authored value, not machinery: the modifier that holds them is deletable, the
// editor shows these rows in the condition widget, and every number below is a
// one-line edit there. Nothing re-creates them.
//
// SLOPE IS AUTHORED IN DEGREES, deliberately, and this is the trap the choice
// avoids. The normalized slope domain (`1 - max(N.y, 0)`) is taken off a
// heightfield whose samples are normalized to [0,1] but differenced over METRE
// spacing, so a threshold in it is divided by the terrain's HeightScale and means
// a different real angle on every terrain: a band that reads as 33.6 degrees at
// HeightScale 1 sits at 88.6 at HeightScale 60, where no ground reaches it and
// the row silently paints nothing. On a measured 512 m terrain the steepest texel
// reaches 0.000129 in that domain against a 0.1667 threshold — a 1292x shortfall.
// SlopeDegrees has no such dependence, and these rows are placed against a
// MEASURED distribution (TerrainDefaultSurfaceRulesTests asserts every band still
// falls inside it). The snow row is placed in meters from the terrain's own
// height range, for the scale reason given on its constants.
//
// ROW ORDER IS LOAD-BEARING. CompositeSplatTexel renormalizes as each row lands,
// so where two rows are both live the LATER one dominates. The order is base
// coverage first, then accents, then the cap: grass, dirt, rock, snow. Reordering
// the rows in the widget changes the surface — that is the intended control, not
// a hazard to design around.

// Slope bands, in degrees from horizontal. Placed against the MEASURED slope
// distribution of a 512 m terrain (TerrainDefaultSurfaceRulesTests prints it):
// p50 13.5, p75 18.8, p90 23.9, p95 26.9, p99 32.5, max 44.0 degrees.
inline constexpr float32 kDefaultGrassSlopeMax = 16.0f;     // full grass over the gentle half
inline constexpr float32 kDefaultGrassSlopeFeather = 10.0f; // gone by 26 deg, the p95 slope
inline constexpr float32 kDefaultDirtSlopeMin = 18.0f;      // the p75 shoulder, where soil thins
inline constexpr float32 kDefaultDirtSlopeMax = 27.0f;      // to the p95
inline constexpr float32 kDefaultDirtSlopeFeather = 6.0f;
inline constexpr float32 kDefaultRockSlopeMin = 30.0f;      // the p99: the steepest ground only
inline constexpr float32 kDefaultRockSlopeFeather = 9.0f;   // blending in from the p90

// Snow: an altitude cap, with a slope limit so it does not cling to cliff faces.
//
// New terrains do not share a height range, so one fixed snow line in meters would
// sit at the summit on one terrain and under water on the next. The row is placed
// in meters at these fractions of the terrain's height range, at creation and again
// while the rows are untouched (PlaceProvisionedSurfaceRules); after an edit it is an
// ordinary meters row. Measured fraction: p50 0.597, p75 0.709, p90 0.798, p95 0.849.
inline constexpr float32 kDefaultSnowRangeFractionMin = 0.85f;     // full snow above the p95
inline constexpr float32 kDefaultSnowRangeFractionFeather = 0.15f; // blending down to the p75
inline constexpr float32 kDefaultSnowSlopeMax = 25.0f;
inline constexpr float32 kDefaultSnowSlopeFeather = 8.0f;

// The far edge of a one-sided band. A band is [Min, Max] with ramps OUTSIDE each
// edge, so a one-sided band puts its unused edge past the domain: slope cannot
// exceed 90 degrees, and no terrain is this tall (meters).
inline constexpr float32 kDefaultSlopeDomainMax = 90.0f;
inline constexpr float32 kDefaultHeightDomainMaxMeters = 100000.0f;

// Splat channels, in the order the shipped default palette binds them
// (Terrain::kDefaultTerrainMaterials): 0 grass, 1 rock, 2 dirt, 3 snow.
inline constexpr uint32 kDefaultGrassSlot = 0;
inline constexpr uint32 kDefaultRockSlot = 1;
inline constexpr uint32 kDefaultDirtSlot = 2;
inline constexpr uint32 kDefaultSnowSlot = 3;

inline Components::TerrainRuleCondition SlopeBand(float32 minDeg, float32 maxDeg, float32 feather)
{
    Components::TerrainRuleCondition c{};
    c.Kind = Components::TerrainRuleConditionKind::SlopeDegrees;
    c.FalloffCurve = Components::TerrainRuleFalloffCurve::ClampedLinear;
    c.Min = minDeg;
    c.Max = maxDeg;
    c.Feather = feather;
    return c;
}

inline Components::TerrainRuleCondition HeightBandMeters(float32 min, float32 max, float32 feather)
{
    Components::TerrainRuleCondition c{};
    c.Kind = Components::TerrainRuleConditionKind::HeightMeters;
    c.FalloffCurve = Components::TerrainRuleFalloffCurve::ClampedLinear;
    c.Min = min;
    c.Max = max;
    c.Feather = feather;
    return c;
}

/// The default rows for a terrain whose height range is [minHeightMeters, maxHeightMeters]
/// above its base (TerrainService::GetHeightRangeMeters). Returns the whole effect so the
/// caller cannot forget RuleCount. The effect is marked ProvisionedDefaults.
inline Components::TerrainSurfaceRulesEffect MakeDefaultTerrainSurfaceRules(float32 minHeightMeters,
                                                                            float32 maxHeightMeters)
{
    Components::TerrainSurfaceRulesEffect fx{};
    fx.RuleCount = 4;
    fx.ProvisionedDefaults = true;

    // 0 · Grass — the base coverage. Everything flat enough to hold soil.
    fx.Rules[0].MaterialSlot = kDefaultGrassSlot;
    fx.Rules[0].ConditionCount = 1;
    fx.Rules[0].Conditions[0] =
        SlopeBand(0.0f, kDefaultGrassSlopeMax, kDefaultGrassSlopeFeather);

    // 1 · Dirt — the shoulder between grass and bare rock, where soil thins but
    // has not gone. A band rather than a ceiling, so it reads as a transition.
    fx.Rules[1].MaterialSlot = kDefaultDirtSlot;
    fx.Rules[1].ConditionCount = 1;
    fx.Rules[1].Conditions[0] =
        SlopeBand(kDefaultDirtSlopeMin, kDefaultDirtSlopeMax, kDefaultDirtSlopeFeather);

    // 2 · Rock — steep ground, and steeper wins over the shoulder because this
    // row lands after it.
    fx.Rules[2].MaterialSlot = kDefaultRockSlot;
    fx.Rules[2].ConditionCount = 1;
    fx.Rules[2].Conditions[0] =
        SlopeBand(kDefaultRockSlopeMin, kDefaultSlopeDomainMax, kDefaultRockSlopeFeather);

    // 3 · Snow — altitude, capped by slope so it does not stick to cliff faces.
    // Last, so the summit reads as snow over whatever else is live there.
    fx.Rules[3].MaterialSlot = kDefaultSnowSlot;
    fx.Rules[3].ConditionCount = 2;
    const float32 heightRange = maxHeightMeters - minHeightMeters;
    fx.Rules[3].Conditions[0] =
        HeightBandMeters(minHeightMeters + kDefaultSnowRangeFractionMin * heightRange,
                         kDefaultHeightDomainMaxMeters, kDefaultSnowRangeFractionFeather * heightRange);
    fx.Rules[3].Conditions[1] =
        SlopeBand(0.0f, kDefaultSnowSlopeMax, kDefaultSnowSlopeFeather);

    return fx;
}

// The planet's default rows equal a slope-and-altitude blend (its oracle is in
// TerrainPlanetSurfaceRulesTests.cpp): with Replace rows in the order grass, dirt, snow, rock,
// rock = r, snow = (1 - r) s, dirt = (1 - r)(1 - s) d, grass = the rest. The height bands
// are fractions of the planet's relief range in meters, measured from its lowest ground.
inline constexpr float32 kPlanetDirtRangeFractionMin = 0.70f;
inline constexpr float32 kPlanetDirtRangeFractionFeather = 0.25f;
inline constexpr float32 kPlanetSnowRangeFractionMin = 0.90f;
inline constexpr float32 kPlanetSnowRangeFractionFeather = 0.175f;
inline constexpr float32 kPlanetRockSlopeMin = 0.65f;
inline constexpr float32 kPlanetRockSlopeFeather = 0.35f;

inline Components::TerrainRuleCondition PlanetBandCondition(Components::TerrainRuleConditionKind kind,
                                                            float32 min, float32 max, float32 feather)
{
    Components::TerrainRuleCondition c{};
    c.Kind = kind;
    c.FalloffCurve = Components::TerrainRuleFalloffCurve::Smoothstep;
    c.Min = min;
    c.Max = max;
    c.Feather = feather;
    return c;
}

// A Replace row with one Smoothstep band: the shape of every planet default row.
inline Components::TerrainSurfaceRule PlanetBandRule(uint32 materialSlot,
                                                     Components::TerrainRuleConditionKind kind,
                                                     float32 min, float32 max, float32 feather)
{
    Components::TerrainSurfaceRule rule{};
    rule.MaterialSlot = materialSlot;
    rule.Replace = true;
    rule.ConditionCount = 1;
    rule.Conditions[0] = PlanetBandCondition(kind, min, max, feather);
    return rule;
}

/// The default rows for a planet whose relief spans [minHeightMeters, maxHeightMeters] about
/// its radius (PlanetHeightRangeMeters).
inline Components::TerrainSurfaceRulesEffect MakeDefaultPlanetSurfaceRules(float32 minHeightMeters,
                                                                           float32 maxHeightMeters)
{
    using Kind = Components::TerrainRuleConditionKind;
    const float32 heightRange = maxHeightMeters - minHeightMeters;
    Components::TerrainSurfaceRulesEffect fx{};
    fx.RuleCount = 4;
    fx.Rules[0].MaterialSlot = kDefaultGrassSlot;
    fx.Rules[0].Replace = true;
    fx.Rules[1] = PlanetBandRule(kDefaultDirtSlot, Kind::HeightMeters,
                                 minHeightMeters + kPlanetDirtRangeFractionMin * heightRange,
                                 kDefaultHeightDomainMaxMeters, kPlanetDirtRangeFractionFeather * heightRange);
    fx.Rules[2] = PlanetBandRule(kDefaultSnowSlot, Kind::HeightMeters,
                                 minHeightMeters + kPlanetSnowRangeFractionMin * heightRange,
                                 kDefaultHeightDomainMaxMeters, kPlanetSnowRangeFractionFeather * heightRange);
    fx.Rules[3] = PlanetBandRule(kDefaultRockSlot, Kind::SlopeNormalized, kPlanetRockSlopeMin,
                                 1.0f, kPlanetRockSlopeFeather);
    return fx;
}

} // namespace GameEngine::TerrainECS
