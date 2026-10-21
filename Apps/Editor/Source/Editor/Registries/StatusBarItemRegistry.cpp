#include "Editor/Registries/StatusBarItemRegistry.h"

#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kContainerId = "StatusBarItems";
constexpr const char* kLabelIdPrefix = "StatusBarItem.";
constexpr const char* kLabelClass = "bottom-toolbar-status";
constexpr const char* kItemClass = "bottom-toolbar-status-item";
constexpr const char* kHiddenClass = "hidden";

Label* FindOrCreateLabel(UIElement& container, const std::string& itemId)
{
    const std::string labelId = kLabelIdPrefix + itemId;
    if (auto* label = dynamic_cast<Label*>(container.FindById(HashStringId(labelId))))
        return label;
    auto owned = std::make_unique<Label>();
    owned->SetId(labelId);
    owned->AddClass(kLabelClass);
    owned->AddClass(kItemClass);
    owned->AddClass(kHiddenClass);
    Label* const label = owned.get();
    container.AddChild(std::move(owned));
    return label;
}
} // namespace

void StatusBarItemRegistry::Register(StatusBarItem item)
{
    // Two items under one id would share one label, and one item's text would never show.
    const bool duplicate = std::any_of(m_Items.begin(), m_Items.end(),
                                       [&item](const StatusBarItem& existing) { return existing.Id == item.Id; });
    if (duplicate)
    {
        Logger::Log::Error("Status bar item '{}' is already registered; give each item its own id.", item.Id);
        assert(!duplicate && "a status bar item id is registered once");
        return;
    }
    m_Items.push_back(std::move(item));
}

void StatusBarItemRegistry::Update(std::chrono::duration<double> elapsed, UIElement* root)
{
    m_SinceLastPoll += elapsed;
    if (m_SinceLastPoll < kPollInterval)
        return;
    m_SinceLastPoll = std::chrono::duration<double>::zero();

    UIElement* const container = root ? root->FindById(HashStringId(kContainerId)) : nullptr;
    if (!container)
        return;
    for (const StatusBarItem& item : m_Items)
    {
        StatusBarItemDisplay display = item.Display();
        Label* const label = FindOrCreateLabel(*container, item.Id);
        label->SetText(display.Text);
        if (label->GetTooltip() != display.Tooltip)
            label->SetTooltip(std::move(display.Tooltip));
        if (display.Text.empty())
            label->AddClass(kHiddenClass);
        else
            label->RemoveClass(kHiddenClass);
    }
}

} // namespace GameEngine::Editor
