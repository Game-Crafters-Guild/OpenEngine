#pragma once

#include "Components/Spline/SplineFence.h"
#include "UI/Controls/Dropdown.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

// The fields both fence inspectors show for one span override: the scene
// view's span section and the fence's override list name the kinds, the parts
// and the problems the same way.

// The kinds of the reflected SplineSpanOverride::Kind by their display names, valued by the
// enumerator. `noneLabel` names the None entry; an empty `noneLabel` leaves None out.
std::vector<Dropdown::Option> SpanOverrideKindOptions(std::string_view noneLabel);
// The option of `kind` in `options`, or 0.
int SpanOverrideKindIndex(const std::vector<Dropdown::Option>& options, Components::SplineSpanOverrideKind kind);
// The kind that an option value of `options` names.
std::optional<Components::SplineSpanOverrideKind> SpanOverrideKindOfOption(const std::vector<Dropdown::Option>& options,
                                                                          std::string_view value);

// The display name of a part kind, as the Parts list's kind dropdown shows it.
std::string FencePartKindName(Components::SplineFencePartKind kind);

// The options of an override's part dropdown and the selected one.
struct OverridePartChoices
{
    std::vector<Dropdown::Option> Options;
    int Selected = 0;
};

// The fence's parts of `kind` with a mesh, each named by its kind and its mesh, valued by its pool
// slot. A second part of one kind and mesh adds a count to its name. A `slot` that the pool does not
// hold adds a "Missing part" option. The option of `slot` is selected.
OverridePartChoices OverridePartOptions(const Components::SplineFence& fence, Components::SplineFencePartKind kind,
                                        uint8 slot);
// The pool slot that an option of OverridePartOptions names.
std::optional<uint8> OverridePartOfOption(std::string_view value);

// A dropdown row that offers OverridePartOptions. `onPicked` receives the chosen pool slot.
Dropdown* AddOverridePartRow(UIElement* parent, const std::string& label, const Components::SplineFence& fence,
                             Components::SplineFencePartKind kind, uint8 slot, const char* tooltip,
                             std::function<void(uint8)> onPicked);

// The state of the fence's last build: whether it placed the fence, and the spans it placed.
struct FenceBuildState
{
    // True when the last build placed the fence. Only then do StandingSpans say which spans exist.
    bool Built = false;
    // The spans that the fence's parts stand in, as (point, span).
    std::span<const std::pair<uint32, uint32>> StandingSpans;
    // The segments of the spline, when its data is loaded.
    std::optional<uint32> SegmentCount;
};

// What the override in `slot` of `fence` cannot do, in sentences that name the override by its
// span, or empty. A span that does not exist is named only when `build` holds a build.
std::string SpanOverrideProblem(const Components::SplineFence& fence, uint32 slot, const FenceBuildState& build);

// True when the override in `slot` of `fence` can place its part, but the last build in `build` placed
// no span at the override's point and span, so the override places nothing. False with no build.
bool SpanOverrideMissesItsSpan(const Components::SplineFence& fence, uint32 slot, const FenceBuildState& build);

} // namespace GameEngine::Editor
