#include "TerrainECS/TerrainSurface/TerrainSurfaceRulePresets.h"

#include "TerrainECS/TerrainDefaultSurfaceRules.h"

#include <iterator>

namespace GameEngine::TerrainECS
{
namespace
{

using Components::TerrainDomain;
using Components::TerrainSurfaceRulesEffect;
using Kind = Components::TerrainRuleConditionKind;

// Polar caps begin at 70 degrees of latitude and fade in over 8 degrees below it.
constexpr float32 kPolarCapLatitudeMin = 70.0f;
constexpr float32 kPolarCapLatitudeFeather = 8.0f;
constexpr float32 kLatitudeDegreesMax = 90.0f;
// Temperate rock in degrees: the default rows' raw band 0.30 to 0.65 is about 46 to 70 degrees.
constexpr float32 kTemperateRockSlopeMinDegrees = 70.0f;
constexpr float32 kTemperateRockSlopeFeatherDegrees = 24.0f;
constexpr float32 kSlopeDegreesMax = 90.0f;

// Steep ground in degrees: full rock from 57 degrees, fading in from 32 degrees.
constexpr float32 kSteepSlopeMinDegrees = 57.0f;
constexpr float32 kSteepSlopeFeatherDegrees = 25.0f;
// Desert summits: the top of the relief range reads as bare rock. Fractions of the range.
constexpr float32 kDesertRockRangeFractionMin = 0.85f;
constexpr float32 kDesertRockRangeFractionFeather = 0.15f;
// Barren lowlands: dust collects in the bottom of the relief range on gentle ground.
constexpr float32 kBarrenDustRangeFractionMax = 0.4f;
constexpr float32 kBarrenDustRangeFractionFeather = 0.2f;
// Dust on ground up to 46 degrees, gone by 60 degrees.
constexpr float32 kBarrenDustSlopeMaxDegrees = 46.0f;
constexpr float32 kBarrenDustSlopeFeatherDegrees = 14.0f;

TerrainSurfaceRulesEffect BaseRows(uint32 baseSlot)
{
    TerrainSurfaceRulesEffect fx{};
    fx.RuleCount = 1;
    fx.Rules[0].MaterialSlot = baseSlot;
    fx.Rules[0].Replace = true;
    return fx;
}

void AppendRule(TerrainSurfaceRulesEffect& fx, const Components::TerrainSurfaceRule& rule)
{
    fx.Rules[fx.RuleCount++] = rule;
}

Components::TerrainSurfaceRule SteepRock()
{
    return PlanetBandRule(kDefaultRockSlot, Kind::SlopeDegrees, kSteepSlopeMinDegrees, kSlopeDegreesMax,
                          kSteepSlopeFeatherDegrees);
}

// The default planet rows with polar snow before rock, so cliffs at the poles stay rock. The
// rock row reads slope in degrees; the default rows keep raw slope to equal the planet shading.
TerrainSurfaceRulesEffect MakeTemperatePlanetRules(float32 minHeightMeters, float32 maxHeightMeters)
{
    TerrainSurfaceRulesEffect fx = MakeDefaultPlanetSurfaceRules(minHeightMeters, maxHeightMeters);
    fx.Rules[fx.RuleCount - 1] = PlanetBandRule(kDefaultSnowSlot, Kind::LatitudeDegrees,
                                                kPolarCapLatitudeMin, kLatitudeDegreesMax,
                                                kPolarCapLatitudeFeather);
    AppendRule(fx, PlanetBandRule(kDefaultRockSlot, Kind::SlopeDegrees, kTemperateRockSlopeMinDegrees,
                                  kSlopeDegreesMax, kTemperateRockSlopeFeatherDegrees));
    return fx;
}

TerrainSurfaceRulesEffect MakeDesertPlanetRules(float32 minHeightMeters, float32 maxHeightMeters)
{
    const float32 heightRange = maxHeightMeters - minHeightMeters;
    TerrainSurfaceRulesEffect fx = BaseRows(kDefaultDirtSlot);
    AppendRule(fx, PlanetBandRule(kDefaultRockSlot, Kind::HeightMeters,
                                  minHeightMeters + kDesertRockRangeFractionMin * heightRange,
                                  kDefaultHeightDomainMaxMeters, kDesertRockRangeFractionFeather * heightRange));
    AppendRule(fx, SteepRock());
    return fx;
}

TerrainSurfaceRulesEffect MakeIceMoonPlanetRules(float32, float32)
{
    TerrainSurfaceRulesEffect fx = BaseRows(kDefaultSnowSlot);
    AppendRule(fx, SteepRock());
    return fx;
}

TerrainSurfaceRulesEffect MakeBarrenPlanetRules(float32 minHeightMeters, float32 maxHeightMeters)
{
    const float32 heightRange = maxHeightMeters - minHeightMeters;
    TerrainSurfaceRulesEffect fx = BaseRows(kDefaultRockSlot);
    Components::TerrainSurfaceRule dust =
        PlanetBandRule(kDefaultDirtSlot, Kind::HeightMeters, minHeightMeters,
                       minHeightMeters + kBarrenDustRangeFractionMax * heightRange,
                       kBarrenDustRangeFractionFeather * heightRange);
    dust.ConditionCount = 2;
    dust.Conditions[1] = PlanetBandCondition(Kind::SlopeDegrees, 0.0f, kBarrenDustSlopeMaxDegrees,
                                             kBarrenDustSlopeFeatherDegrees);
    AppendRule(fx, dust);
    return fx;
}

constexpr TerrainSurfaceRulePreset kPresets[] = {
    {"Temperate", "Grass on gentle ground, dirt on shoulders, rock on steep ground, snow on high ground.",
     TerrainDomain::Planar, MakeDefaultTerrainSurfaceRules},
    {"Temperate", "Grass in the lowlands, dirt and snow with height, polar snow caps, rock on steep ground.",
     TerrainDomain::Spherical, MakeTemperatePlanetRules},
    {"Desert", "Dirt everywhere, rock on summits and steep ground, no snow.",
     TerrainDomain::Spherical, MakeDesertPlanetRules},
    {"Ice Moon", "Snow everywhere, rock on steep ground.",
     TerrainDomain::Spherical, MakeIceMoonPlanetRules},
    {"Barren", "Rock everywhere, dirt in gentle lowlands.",
     TerrainDomain::Spherical, MakeBarrenPlanetRules},
};

} // namespace

std::span<const TerrainSurfaceRulePreset> TerrainSurfaceRulePresets()
{
    return kPresets;
}

} // namespace GameEngine::TerrainECS
