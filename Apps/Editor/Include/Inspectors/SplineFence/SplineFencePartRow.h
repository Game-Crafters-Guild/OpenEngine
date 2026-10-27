#pragma once

#include "UI/Controls/ListItemActions.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <functional>

namespace GameEngine
{
class GUID;
class IThumbnailProvider;
} // namespace GameEngine

namespace GameEngine::Components
{
enum class SplineFencePartKind : uint8_t;
struct SplineFencePart;
} // namespace GameEngine::Components

namespace GameEngine::EditorUI
{

/// One row of a fence's Parts list: the part's kind as a dropdown, then its mesh and the list
/// actions, on one line. The layout is
/// UI/inspectors/SplineFencePartRow/SplineFencePartRow.uxml and the look its .css.
class SplineFencePartRow final : public UIElement
{
  public:
    struct Handlers
    {
        std::function<void(Components::SplineFencePartKind)> KindChanged;
        std::function<void(const GUID&)> MeshChanged;
        ListItemActions::Handlers Actions;
    };

    /// Shows `part`. Move Up is off when `first` and Move Down when `last`. `thumbnails` can be null.
    SplineFencePartRow(const Components::SplineFencePart& part, bool first, bool last,
                       IThumbnailProvider* thumbnails, Handlers handlers);
};

} // namespace GameEngine::EditorUI
