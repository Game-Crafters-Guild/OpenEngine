#pragma once

// TerrainGrassClamps.h — the TerrainGrass DATA invariants, in one place.
//
// These are the bounds a stored component must satisfy however it was written, so every surface
// that lands raw values on a TerrainGrass ends by running this pass: the scene schema
// (TerrainGrassSceneSchemas.cpp) after parsing a property, and the debug server's set_component
// after the generic reflected write. A second hand-maintained copy is how two authoring paths
// come to disagree about what a valid component is.
//
// This is not every bound in the editor, and deliberately so. TerrainInspector.cpp carries its
// own per-widget limits — slider travel, step size, display ranges — which are choices about how
// far a drag goes, not statements about which values are storable; those stay with the widgets.
// What belongs here is the subset that is true of the DATA: the per-field ranges below (the two
// densities are capped at the per-cell slot ceiling, past which a value cannot render), plus the
// atlas invariant relating three fields to each other, which the inspector enforced alone until
// every other writer could set a tile count no atlas grid could address.

#include "Components/Terrain/TerrainGrass.h"
#include "Mathematics/Interpolation.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainGrass/GrassPlacementModel.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainGrass
{

// A gust scale of zero divides in the wind noise lookup; this is the smallest scale the
// shader can evaluate, not a taste bound.
inline constexpr float32 kMinWindGustScale = 0.0001f;
// Atlas grid bounds. One tile is the degenerate "whole texture is one card" case; the
// ceiling is the widest grid the card-atlas UV packing addresses.
inline constexpr uint32 kMinGrassAtlasDimension = 1u;
inline constexpr uint32 kMaxGrassAtlasDimension = 16u;
inline constexpr uint32 kMinGrassAtlasTileCount = 1u;
// Twice the physical canopy shade: the most the dial asks for. The depth itself is capped
// at kTerrainGrassCanopyOcclusionMax whatever the dial says.
inline constexpr float32 kMaxGrassCanopyShade = 2.0f;

// Bring every field of `grass` inside its valid range.
//
// Whole-component rather than per-field, and idempotent, so a caller runs it once after a
// write of any shape — one field, several, or a raw byte blob — without tracking which
// fields it touched. Fields with no meaningful bound (seeds, directions, colours, asset
// refs, the enum) are deliberately absent.
inline void ClampFields(Components::TerrainGrass& grass)
{
    grass.BladesPerSquareMeter =
        std::clamp(grass.BladesPerSquareMeter, 0.0f, kGrassMaxBladesPerSquareMeter);
    grass.Range = std::max(0.0f, grass.Range);
    grass.DensityFalloff = std::max(0.0f, grass.DensityFalloff);
    grass.ClumpSize = std::max(0.0f, grass.ClumpSize);
    grass.ClumpHeightVariance = Math::Clamp01(grass.ClumpHeightVariance);
    grass.ClumpAlignment = Math::Clamp01(grass.ClumpAlignment);
    grass.ClumpTaper = Math::Clamp01(grass.ClumpTaper);
    grass.BladeHeight = std::max(0.0f, grass.BladeHeight);
    grass.BladeWidth = std::max(0.0f, grass.BladeWidth);
    grass.BladeTilt = std::clamp(grass.BladeTilt, 0.0f, Components::kMaxTerrainGrassBladeTilt);
    grass.BladeBend = std::clamp(grass.BladeBend, 0.0f, Components::kMaxTerrainGrassBladeBend);
    grass.BladeTwist = std::clamp(grass.BladeTwist, 0.0f, Components::kMaxTerrainGrassBladeTwist);
    grass.BladeTaperStart =
        std::clamp(grass.BladeTaperStart, 0.0f, Components::kMaxTerrainGrassBladeTaperStart);
    grass.TuftFan = std::clamp(grass.TuftFan, 0.0f, Components::kMaxTerrainGrassTuftFan);
    grass.BladeSegments = std::clamp(grass.BladeSegments,
                                     Components::kMinTerrainGrassBladeSegments,
                                     Components::kMaxTerrainGrassBladeSegments);
    grass.RandomScale = std::max(0.0f, grass.RandomScale);
    grass.LayerIndex = std::min(grass.LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u);
    grass.MaskThreshold = Math::Clamp01(grass.MaskThreshold);
    grass.WindGustSpeed = std::max(0.0f, grass.WindGustSpeed);
    grass.WindGustScale = std::max(kMinWindGustScale, grass.WindGustScale);
    grass.WindStrength = std::max(0.0f, grass.WindStrength);
    grass.WindFlutterAmount = std::max(0.0f, grass.WindFlutterAmount);
    grass.WindFlutterSpeed = std::max(0.0f, grass.WindFlutterSpeed);
    grass.Brightness = std::max(0.0f, grass.Brightness);
    grass.RandomBrightness = std::max(0.0f, grass.RandomBrightness);
    grass.HueVariation = Math::Clamp01(grass.HueVariation);
    grass.RootFadeStart = std::clamp(grass.RootFadeStart, Components::kMinTerrainGrassRootFadeStart,
                                     1.0f - Components::kMinTerrainGrassRootFadeSpan);
    grass.RootFadeEnd = std::clamp(grass.RootFadeEnd,
                                   grass.RootFadeStart + Components::kMinTerrainGrassRootFadeSpan, 1.0f);
    grass.GroundingStrength = Math::Clamp01(grass.GroundingStrength);
    grass.GroundTintHeight = std::clamp(grass.GroundTintHeight, Components::kMinTerrainGrassGroundTintHeight, 1.0f);
    grass.CanopyShade = std::clamp(grass.CanopyShade, 0.0f, kMaxGrassCanopyShade);
    grass.GroundDarkening = std::isfinite(grass.GroundDarkening) ? Math::Clamp01(grass.GroundDarkening) : 0.0f;
    grass.Translucency = std::clamp(grass.Translucency, 0.0f, Components::kMaxTerrainGrassTranslucency);
    grass.TextureCardsPerSquareMeter =
        std::clamp(grass.TextureCardsPerSquareMeter, 0.0f, kGrassMaxBladesPerSquareMeter);
    grass.TextureSize = std::max(0.0f, grass.TextureSize);
    grass.AtlasColumns =
        std::clamp(grass.AtlasColumns, kMinGrassAtlasDimension, kMaxGrassAtlasDimension);
    grass.AtlasRows =
        std::clamp(grass.AtlasRows, kMinGrassAtlasDimension, kMaxGrassAtlasDimension);
    // Cross-field, and the reason this pass is whole-component: a tile count is an index space
    // into the grid the other two fields define, so it is only meaningful against them. Clamped
    // after both, so the ceiling is the corrected grid rather than whatever arrived.
    grass.AtlasTileCount = std::clamp(grass.AtlasTileCount, kMinGrassAtlasTileCount,
                                      grass.AtlasColumns * grass.AtlasRows);
    grass.AlphaCutoff = Math::Clamp01(grass.AlphaCutoff);
    grass.NormalStrength = std::max(0.0f, grass.NormalStrength);
}

} // namespace GameEngine::TerrainGrass
