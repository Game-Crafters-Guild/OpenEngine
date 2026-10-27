#include "UI/Controls/ListItemActions.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/ListItemActions/ListItemActions.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/ListItemActions/ListItemActions.css";

// Calls `handler` when one is set.
void RunHandler(const std::function<void()>& handler)
{
    if (handler)
        handler();
}

} // namespace

ListItemActions::ListItemActions()
{
    AddClass("list-item-actions");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> group = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!group)
        return;
    m_MoveUp = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(group.get(), "list-item-move-up"));
    m_MoveDown = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(group.get(), "list-item-move-down"));
    auto* remove = dynamic_cast<Button*>(InspectorDrag::FindChildByClass(group.get(), "list-item-remove"));
    if (!m_MoveUp || !m_MoveDown || !remove)
    {
        Logger::Log::Error("ListItemActions: {} needs the three action buttons.", kLayoutAssetPath);
        m_MoveUp = nullptr;
        m_MoveDown = nullptr;
        return;
    }
    m_MoveUp->SetTooltip("Move Up");
    m_MoveDown->SetTooltip("Move Down");
    remove->SetTooltip("Remove");
    m_MoveUp->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { RunHandler(m_Handlers.MoveUp); });
    m_MoveDown->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { RunHandler(m_Handlers.MoveDown); });
    remove->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { RunHandler(m_Handlers.Remove); });
    AddChild(std::move(group));
}

void ListItemActions::SetHandlers(Handlers handlers, bool first, bool last)
{
    m_Handlers = std::move(handlers);
    if (m_MoveUp)
        m_MoveUp->SetEnabled(!first);
    if (m_MoveDown)
        m_MoveDown->SetEnabled(!last);
}

} // namespace GameEngine::EditorUI

namespace RegisterListItemActions
{
static auto s_RegisterListItemActions =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorUI::ListItemActions>(
        "ListItemActions", []() { return std::make_unique<GameEngine::EditorUI::ListItemActions>(); });
}
