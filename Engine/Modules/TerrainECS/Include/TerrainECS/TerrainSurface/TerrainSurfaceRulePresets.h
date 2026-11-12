#pragma once

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"

#include <span>
#include <string_view>

namespace GameEngine::TerrainECS
{

/// A named rule set a terrain starts from. Applying one replaces a rules block's rows;
/// the rows stay the stored value, so a preset is never saved and never re-applied.
struct TerrainSurfaceRulePreset
{
    std::string_view Name;
    std::string_view Description;
    /// The terrain domain the rows are written for: their kinds read that domain's measures.
    Components::TerrainDomain Domain = Components::TerrainDomain::Planar;
    /// The rows for a terrain whose height range is [minHeightMeters, maxHeightMeters]: above
    /// the base on a flat terrain (TerrainService::GetHeightRangeMeters), about the radius on
    /// a planet (PlanetHeightRangeMeters).
    Components::TerrainSurfaceRulesEffect (*Make)(float32 minHeightMeters, float32 maxHeightMeters) = nullptr;
};

/// Every rule-set preset, planar first, in the order the inspector lists them.
std::span<const TerrainSurfaceRulePreset> TerrainSurfaceRulePresets();

} // namespace GameEngine::TerrainECS
