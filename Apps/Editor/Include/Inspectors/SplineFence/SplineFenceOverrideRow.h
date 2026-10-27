#pragma once

#include "UI/Controls/ListItemActions.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <string>

namespace GameEngine::Components
{
enum class SplineSpanOverrideKind : uint8_t;
struct SplineFence;
} // namespace GameEngine::Components

namespace GameEngine::EditorUI
{

/// One item of a fence's Span Overrides list: the kind, the part and the list actions, then the point,
/// the span and the override's problem when it has one. The layout and the look are
/// UI/inspectors/SplineFenceOverrideRow/SplineFenceOverrideRow.uxml and its .css.
class SplineFenceOverrideRow final : public UIElement
{
  public:
    struct Handlers
    {
        std::function<void(Components::SplineSpanOverrideKind)> KindChanged;
        std::function<void(uint32_t)> PointChanged;
        std::function<void(uint16_t)> SpanChanged;
        std::function<void(uint8_t)> PartChanged;
        ListItemActions::Handlers Actions;
    };

    /// Shows the override in `slot` of `fence`. Move Up is off when `first` and Move Down when
    /// `last`. An empty `problem` shows no notice.
    SplineFenceOverrideRow(const Components::SplineFence& fence, uint32_t slot, bool first, bool last,
                           const std::string& problem, Handlers handlers);
};

} // namespace GameEngine::EditorUI
