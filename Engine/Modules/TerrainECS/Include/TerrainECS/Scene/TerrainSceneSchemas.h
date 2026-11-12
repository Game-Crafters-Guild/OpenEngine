#pragma once

#include "Types/Types.h"

#include <string>
#include <string_view>

namespace GameEngine::Scene
{

// Ensures TerrainECS scene component schemas are registered even when static
// schema auto-registration is stripped from a linked static library.
void EnsureTerrainSceneSchemasRegistered();

/// The scene field that names a terrain's base heightmap asset. TerrainService reads that
/// asset's source file by path, so an export ships the source of every asset this field names.
inline constexpr std::string_view kTerrainHeightmapSceneField = "Terrain.terrainAsset";

/// Why a scene load refused a surface rule condition's kind number. The load report and the
/// Inspector's row show the same text. Indices are zero-based; the text counts from 1.
/// The text gives one fix, the Inspector's Measures pick: Height (meters) for kind 3.
std::string RefusedRuleConditionKindMessage(uint32 ruleIndex, uint32 conditionIndex, uint32 refusedKind);

} // namespace GameEngine::Scene
