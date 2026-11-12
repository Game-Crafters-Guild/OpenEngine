#pragma once

// Opens the context menu of an Inspector row through its right-click gesture, as the UI manager delivers it,
// and returns the menu the editor's menu factory builds, as InterceptableContextMenu captures it.

#include "EditorContextMenu/InterceptableContextMenu.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace EditorTesting
{

// Sends one pointer event from `pressed` up through its ancestors until an element claims it.
inline void SendRowPointerEvent(GameEngine::UIElement& pressed, GameEngine::EventId id, int button)
{
    GameEngine::UIEvent event{};
    event.Id = id;
    event.X = 1.0f;
    event.Y = 1.0f;
    event.Button = button;
    event.Target = &pressed;
    for (GameEngine::UIElement* element = &pressed; element; element = element->GetParent())
    {
        event.CurrentTarget = element;
        element->DispatchEvent(event);
        if (event.Handled)
            break;
    }
}

constexpr int kRightMouseButton = 1;

// The menu that a click of `button` on `pressed`, inside `row`, opens; nothing when no menu opens. The row gets a
// layout box that holds the pointer.
inline std::optional<GameEngine::InterceptableContextMenu::Capture> OpenRowContextMenu(
    GameEngine::UIElement& row, GameEngine::UIElement& pressed, int button = kRightMouseButton)
{
    constexpr float kRowWidth = 400.0f;
    constexpr float kRowHeight = 40.0f;
    std::optional<GameEngine::InterceptableContextMenu::Capture> captured;
    GameEngine::InterceptableContextMenu::SetInterceptor(
        [&captured](GameEngine::InterceptableContextMenu::Capture&& capture)
        {
            captured = std::move(capture);
            return true;
        });
    GameEngine::ContextMenuManipulator::SetMenuFactory([] { return GameEngine::CreateContextMenu(); });
    {
        GameEngine::UI::UiDispatcher dispatcher;
        GameEngine::UI::UiContextScope scope{&dispatcher, nullptr};
        GameEngine::UILayoutAccess::SetLastLayoutRect(row, 0.0f, 0.0f, kRowWidth, kRowHeight);
        SendRowPointerEvent(pressed, GameEngine::kEventMouseDown, button);
        SendRowPointerEvent(pressed, GameEngine::kEventMouseUp, button);
        dispatcher.Drain();
    }
    GameEngine::ContextMenuManipulator::SetMenuFactory(nullptr);
    GameEngine::InterceptableContextMenu::SetInterceptor(nullptr);
    return captured;
}

// The rows of a captured menu in order: each item's path, and "---" for a separator.
inline std::vector<std::string> CapturedRows(const GameEngine::InterceptableContextMenu::Capture& capture)
{
    std::vector<std::string> rows;
    for (const auto& item : capture.Items)
        rows.push_back(item.IsSeparator ? std::string("---") : item.Path);
    return rows;
}

// The command of the captured item at `path`; 0 when the menu has no such item.
inline uint32_t CapturedCommand(const GameEngine::InterceptableContextMenu::Capture& capture, const std::string& path)
{
    for (const auto& item : capture.Items)
        if (!item.IsSeparator && item.Path == path)
            return item.CommandId;
    return 0;
}

} // namespace EditorTesting
