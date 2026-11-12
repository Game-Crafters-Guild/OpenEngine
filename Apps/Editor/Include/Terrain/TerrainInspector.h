#pragma once

namespace GameEngine
{
class UIElement;
struct InspectorContext;

void RegisterTerrainInspector();

// Adds a spherical terrain's atmosphere rows under `parent`: the mode (Auto, Share of Radius,
// Height in Meters) and the height in the mode's unit, with the other unit as a read-only caption.
// Under Auto the height is read-only and shows the value TerrainECS::AtmosphereHeightMeters derives.
// A change to Share of Radius or Height in Meters keeps the height; a change to Auto sets it from the relief.
// Each mode change rebuilds the Inspector through ctx.RequestInspectorRefresh.
void AddPlanetAtmosphereRows(UIElement* parent, const InspectorContext& ctx);

} // namespace GameEngine
