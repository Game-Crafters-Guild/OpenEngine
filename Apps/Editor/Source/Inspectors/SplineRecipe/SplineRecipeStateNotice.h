#pragma once

#include "SplineRecipes/SplineRecipesScriptApi.h"
#include "UI/Controls/InspectorNotice.h"

#include <span>
#include <string>
#include <string_view>

namespace GameEngine
{

struct InspectorContext;

// The words and the look of a build state notice.
struct BuildStateWords
{
    std::string Title;
    std::string Text;
    EditorUI::InspectorNotice::Kind Kind = EditorUI::InspectorNotice::Kind::Information;
    EditorUI::InspectorNotice::Glyph Glyph = EditorUI::InspectorNotice::Glyph::OfKind;
};

// The notice of `state`. `reason` is the recipe's reason. `warnings` are the problems of the rows
// below the notice: a Built state with warnings names them, so it never reads as up to date.
BuildStateWords DescribeBuildState(SplineRecipes::BuildState state, std::string_view reason,
                                   std::span<const std::string> warnings);

// Adds the notice of a recipe in `state` with `reason` and the `warnings` of its rows. The notice
// follows the recipe system's state of `context.Entity` while the inspector shows.
void AddBuildStateNotice(const InspectorContext& context, SplineRecipes::BuildState state, std::string_view reason,
                         std::span<const std::string> warnings);

} // namespace GameEngine
