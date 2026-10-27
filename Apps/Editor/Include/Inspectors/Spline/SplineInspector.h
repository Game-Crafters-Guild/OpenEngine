#pragma once

namespace GameEngine
{

struct InspectorContext;

void RegisterSplineInspectors();

// Registers the inspector of the wall: the build state notice above its reflected rows.
void RegisterSplineWallInspector();
// Registers the inspector of the extrude: the build state notice, the width mode, then only the
// rows the width mode and the profile use.
void RegisterSplineExtrudeInspector();

// Adds the notice with the build state of the recipe on `context.Entity` (Extrude, Wall or
// Placement): the state, and the reason when the recipe waits, fails or builds nothing. The notice
// follows the state while the inspector shows.
void AddSplineRecipeBuildStateNotice(const InspectorContext& context);

} // namespace GameEngine
