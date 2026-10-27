#pragma once

#include "UI/UIElement.h"

#include <functional>

namespace GameEngine
{
class Button;
}

namespace GameEngine::EditorUI
{

/// The actions of one item of an inspector list, after the item's fields: Move Up, Move Down and
/// Remove, as icon buttons. A layout declares it as <ListItemActions/>. The look comes from
/// UI/controls/ListItemActions/ListItemActions.css.
class ListItemActions final : public UIElement
{
  public:
    struct Handlers
    {
        std::function<void()> MoveUp;
        std::function<void()> MoveDown;
        std::function<void()> Remove;
    };

    ListItemActions();

    /// Sets what each action does. Move Up is off on the first item and Move Down on the last.
    void SetHandlers(Handlers handlers, bool first, bool last);

  private:
    Handlers m_Handlers;
    Button* m_MoveUp = nullptr;
    Button* m_MoveDown = nullptr;
};

} // namespace GameEngine::EditorUI
