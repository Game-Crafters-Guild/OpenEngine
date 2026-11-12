#include "Terrain/TerrainSizingNotice.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "Types/FormatNumber.h"
#include "Types/StringUtils.h"
#include "UI/Controls/InspectorNotice.h"

#include <cmath>
#include <memory>

namespace GameEngine::Editor
{
namespace
{

// Meters per sample span four orders of magnitude across the legal range (0.25 m on a small
// terrain to 500 m on a coarse far field), so a fixed precision reads as either noise or a lie.
// Whole meters group their digits: "1,048,576 m".
std::string FormatLength(float32 meters)
{
    if (meters >= 100.0f)
        return FormatGroupedWholeNumber(meters) + " m";
    return FormatMeters(meters, meters < 1.0f ? 2 : 1);
}

} // namespace

std::string DescribeTerrainSizing(const TerrainECS::TerrainSizingPlan& plan)
{
    if (plan.UnifiedWidth == 0u)
        return {};

    // Keyed on the pinned pages, not the tile count: a terrain whose whole height pyramid is pinned
    // does not stream, whatever its tile grid says.
    const bool pinned = plan.HeightPages > 0u && plan.PinnedHeightPages == plan.HeightPages;
    const std::string tileGrid = FormatGroupedInteger(plan.TilesPerAxisX) + " x " +
                                 FormatGroupedInteger(plan.TilesPerAxisZ);

    // The terrain draws only its authored size, but its samples and their memory cover whole tiles.
    // This clause goes first so that a short panel shows it. The pinned form names the tile grid
    // here: the clause after it does not.
    std::string text;
    const float32 storedX = static_cast<float32>(plan.TilesPerAxisX) * plan.TileWorldSize;
    const float32 storedZ = static_cast<float32>(plan.TilesPerAxisZ) * plan.TileWorldSize;
    if (storedX > plan.WorldSizeX || storedZ > plan.WorldSizeZ)
    {
        text = "Stores samples for " + FormatLength(storedX) + " x " + FormatLength(storedZ);
        if (pinned)
            text += " (" + tileGrid + " tiles of " + FormatLength(plan.TileWorldSize) + ")";
        text += ", more than it draws. ";
    }

    text += FormatGroupedInteger(plan.UnifiedWidth) + " x " + FormatGroupedInteger(plan.UnifiedHeight) +
            " samples, " + FormatLength(plan.MetresPerTexelNear) + " per sample";
    if (pinned)
        text += " - fits in memory, nothing streams (" +
                FormatCount(plan.HeightPages, "page", "pages") + ")";
    else
        text += " - " + tileGrid + " streamed tiles of " + FormatLength(plan.TileWorldSize);

    return text;
}

std::string DescribeTerrainResidency(const TerrainECS::TerrainSizingPlan& plan)
{
    if (plan.UnifiedWidth == 0u)
        return {};

    const std::string vram = FormatMebibytes(plan.PredictedVramBytes);

    if (plan.Source != TerrainECS::TerrainHeightSource::Atlas)
        return "Costs " + vram + " of terrain textures, all of it at full detail.";

    // Engaging the atlas does NOT by itself cost detail: just past the unified ceiling the slot
    // budget still covers the whole tile grid. Only a grid that outruns the budget has a coarse
    // far field, so the two cases must not share a sentence — saying "beyond the window" of a
    // terrain that has no outside is a contradiction the author would have to disprove.
    if (plan.FullyResident())
        return "Streams all " + FormatGroupedInteger(plan.TotalTiles) + " tiles at full detail for " +
               vram + ". Nothing falls back to the coarse field at this size.";

    // Past that point the window is what stays sharp, and everything beyond it resolves through
    // one coarse field spanning the whole world. That trade is the decision this notice exists
    // to surface.
    return "Streams " + FormatGroupedInteger(plan.ResidentSlots) + " of " +
           FormatGroupedInteger(plan.TotalTiles) + " tiles at full detail (a " +
           FormatLength(plan.ResidentWindowMetres) + " window) for " + vram +
           ". Beyond the window the terrain falls back to " +
           FormatLength(plan.MetresPerTexelFar) +
           " per texel, so distant ground reads smooth rather than detailed.";
}

void AddTerrainSizingNotice(UIElement* parent, float32 sizeX, float32 sizeZ, float32 samplesPerMeter)
{
    // A refused size creates no terrain, so it has nothing to store or stream.
    if (!parent || !TerrainECS::PlanarTerrainSizeError(sizeX, sizeZ, samplesPerMeter).empty())
        return;
    const TerrainECS::TerrainSizingPlan plan = TerrainECS::DeriveTerrainSizingPlan(sizeX, sizeZ, samplesPerMeter);
    if (plan.UnifiedWidth == 0u)
        return;

    InspectorUI::AddInfoCard(parent, DescribeTerrainSizing(plan));

    const std::string residency = DescribeTerrainResidency(plan);
    if (residency.empty())
        return;

    // A partially-resident terrain is a look decision the author has to make knowingly, not a
    // footnote: everything past the window is coarse until they change size, density or budget.
    if (plan.FullyResident())
        InspectorUI::AddInfoCard(parent, residency);
    else
        parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(residency));
}

} // namespace GameEngine::Editor
