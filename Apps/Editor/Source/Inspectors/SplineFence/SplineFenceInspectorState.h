#pragma once

#include "SplineRecipes/SplineRecipesScriptApi.h"

#include <span>
#include <string_view>

namespace GameEngine
{

struct InspectorContext;
namespace SplineRecipes
{
struct FenceMeshBounds;
}

// Builds the fence inspector of `context` for a fence whose last build is in `state` with `reason`
// and measured the part bounds `measured`. The registered inspector passes the recipe system's state.
void BuildSplineFenceInspectorInState(const InspectorContext& context, SplineRecipes::BuildState state,
                                      std::string_view reason,
                                      std::span<const SplineRecipes::FenceMeshBounds> measured);

} // namespace GameEngine
