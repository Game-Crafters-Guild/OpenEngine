#pragma once

namespace GameEngine
{

struct InspectorContext;

void RegisterSplineInspectors();

// Registers the inspectors of the extrude and the wall: the build state notice above their
// reflected rows.
void RegisterSplineRecipeInspectors();

// Adds the notice with the build state of the recipe on `context.Entity` (Extrude, Wall, Fence or
// Placement): the state, and the reason when the recipe waits, fails or builds nothing. The notice
// follows the state while the inspector shows.
void AddSplineRecipeBuildStateNotice(const InspectorContext& context);

} // namespace GameEngine
