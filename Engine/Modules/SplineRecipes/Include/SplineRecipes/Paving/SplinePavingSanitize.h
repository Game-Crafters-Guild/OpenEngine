#pragma once

#include "Components/Spline/SplinePaving.h"

namespace GameEngine::SplineRecipes
{

// Cleans a paving before the controller compares or builds it: bad values and enumerators take a
// valid value, and MemberCount stays at most kMaxPavingMembers with the slots past it cleared, so
// equal recipes compare equal.
Components::SplinePaving SanitizePavingRecipe(const Components::SplinePaving& authored);

// The paving with its look fields (the paving and curb materials and the shadow flags) and its ground
// authority at their defaults: two recipes whose geometry keys are equal build the same meshes. The
// seam course's material stays: it selects the course's tint. A look change
// rebinds the chunks; a ground authority change reaches them through the terrain's surface revision.
Components::SplinePaving PavingGeometryKey(const Components::SplinePaving& recipe);

} // namespace GameEngine::SplineRecipes
