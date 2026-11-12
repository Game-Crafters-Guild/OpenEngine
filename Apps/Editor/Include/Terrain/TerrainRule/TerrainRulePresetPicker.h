#pragma once

#include "Types/Types.h"
#include "UI/Controls/Dropdown.h"

#include <vector>

namespace GameEngine
{

struct InspectorContext;

namespace Components
{
enum class TerrainDomain : uint32;
struct TerrainSurfaceRulesEffect;
} // namespace Components

namespace TerrainECS
{
struct TerrainHeightRangeMeters;
struct TerrainSurfaceRulePreset;
} // namespace TerrainECS

/// The width and top margin of Add Rule and Replace Rules: those of Add Effect one level up.
/// A max-width of 100 percent shrinks them in a narrow Inspector.
inline constexpr float kTerrainRuleActionWidthPx = 360.0f;
inline constexpr float kTerrainRuleActionMarginTopPx = 6.0f;

/// The Replace Rules control's options for a terrain domain: each preset of that domain,
/// valued by its index in TerrainSurfaceRulePresets(). No prompt entry: the prompt is the
/// control's text while nothing is selected.
std::vector<Dropdown::Option> TerrainRulePresetOptions(Components::TerrainDomain domain);

/// Replaces a rules block's rows with the preset's rows for a terrain of that height range.
/// The block's other fields stay; the rows are the author's, so ProvisionedDefaults clears.
void ApplyTerrainRulePreset(Components::TerrainSurfaceRulesEffect& effect,
                            const TerrainECS::TerrainSurfaceRulePreset& preset,
                            const TerrainECS::TerrainHeightRangeMeters& heightRangeMeters);

/// The Replace Rules control, under Add Rule: choosing a preset replaces the block's rows as
/// one undo step.
void AddTerrainRulePresetRow(const InspectorContext& ctx, Components::TerrainDomain domain,
                             const TerrainECS::TerrainHeightRangeMeters& heightRangeMeters);

} // namespace GameEngine
