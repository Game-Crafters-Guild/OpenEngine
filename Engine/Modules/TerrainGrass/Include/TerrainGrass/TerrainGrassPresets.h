#pragma once

// TerrainGrassPresets.h — the kinds of field a game picks with TerrainGrass::Preset: a palette and a
// blade size, density and tilt for each. Picking a preset writes its dials once; they stay editable,
// and an inspector edit that takes one of them away from the picked preset turns Preset to Custom
// (ReconcileTerrainGrassPreset). Preset is a label of where the dials came from: a scene stores and
// loads it as written, and writing a dial by any other path leaves it alone.

#include "Components/Terrain/TerrainGrass.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainGrass
{

// The dials a preset sets. Everything else on the component (wind, clumps, the root fade, Canopy
// Shade, textures) is the scene's own and a preset leaves it alone. Colours are sRGB bytes, as the
// picker writes them; the linear albedo each one decodes to is in its comment.
struct TerrainGrassPresetValues
{
    Components::TerrainGrassPreset Preset;
    uint32 TipColor;
    uint32 RootColor;
    uint32 BacklightColor;
    float32 BladesPerSquareMeter;
    float32 BladeHeight;
    float32 BladeWidth;
    float32 BladeTilt;
};

inline constexpr TerrainGrassPresetValues kTerrainGrassPresets[] = {
    // Meadow, the component's defaults: a leaf green (0.19, 0.30, 0.075) on 0.72 m blades, 21 per
    // square metre at 36 mm so the field covers the ground from eye height and from a strategy
    // camera within the instance budget's 369 m.
    {Components::TerrainGrassPreset::Meadow, 0xFF79954Du, Components::kTerrainGrassAutoRootColor,
     0xFFBCCE61u, 21.0f, 0.72f, 0.036f, 0.25f},
    // Lawn: short, upright and dense, a deeper green (0.13, 0.27, 0.045). 64 per square metre fit
    // the budget to about 211 m.
    {Components::TerrainGrassPreset::Lawn, 0xFF658E3Cu, Components::kTerrainGrassAutoRootColor,
     0xFFAAC859u, 64.0f, 0.4f, 0.024f, 0.1f},
    // Steppe: sparse, tall, wiry and dry, a straw green (0.36, 0.33, 0.12) that leans loosely.
    {Components::TerrainGrassPreset::Steppe, 0xFFA29B61u, Components::kTerrainGrassAutoRootColor,
     0xFFCEC581u, 12.0f, 0.9f, 0.02f, 0.35f},
};

namespace Detail
{
inline const TerrainGrassPresetValues* FindTerrainGrassPreset(Components::TerrainGrassPreset preset)
{
    for (const TerrainGrassPresetValues& values : kTerrainGrassPresets)
    {
        if (values.Preset == preset)
            return &values;
    }
    return nullptr;
}

inline bool TerrainGrassPresetValueMatches(float32 value, float32 preset)
{
    // Tolerates the scene text's %g round trip.
    return std::abs(value - preset) <= 1e-5f * std::max(1.0f, std::abs(preset));
}
} // namespace Detail

// Writes the preset's dials and names it. Custom writes nothing but the name.
inline void ApplyTerrainGrassPreset(Components::TerrainGrass& grass, Components::TerrainGrassPreset preset)
{
    grass.Preset = preset;
    const TerrainGrassPresetValues* values = Detail::FindTerrainGrassPreset(preset);
    if (!values)
        return;
    grass.TipColor = values->TipColor;
    grass.RootColor = values->RootColor;
    grass.BacklightColor = values->BacklightColor;
    grass.BladesPerSquareMeter = values->BladesPerSquareMeter;
    grass.BladeHeight = values->BladeHeight;
    grass.BladeWidth = values->BladeWidth;
    grass.BladeTilt = values->BladeTilt;
}

// Custom as soon as any dial the named preset sets differs from it; otherwise unchanged.
inline void ReconcileTerrainGrassPreset(Components::TerrainGrass& grass)
{
    const TerrainGrassPresetValues* values = Detail::FindTerrainGrassPreset(grass.Preset);
    if (!values)
        return;
    const bool matches = grass.TipColor == values->TipColor && grass.RootColor == values->RootColor
        && grass.BacklightColor == values->BacklightColor
        && Detail::TerrainGrassPresetValueMatches(grass.BladesPerSquareMeter, values->BladesPerSquareMeter)
        && Detail::TerrainGrassPresetValueMatches(grass.BladeHeight, values->BladeHeight)
        && Detail::TerrainGrassPresetValueMatches(grass.BladeWidth, values->BladeWidth)
        && Detail::TerrainGrassPresetValueMatches(grass.BladeTilt, values->BladeTilt);
    if (!matches)
        grass.Preset = Components::TerrainGrassPreset::Custom;
}

} // namespace GameEngine::TerrainGrass
