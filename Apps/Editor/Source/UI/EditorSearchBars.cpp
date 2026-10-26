#include "UI/EditorSearchBars.h"

#include <algorithm>

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "Editor/Settings/SettingsStore.h"

namespace GameEngine {

bool EditorSearchBars::GetAtTop()
{
    if (!s_Loaded)
        LoadFromPreferences();
    return s_AtTop;
}

bool EditorSearchBars::GetAccentFocusOutline()
{
    if (!s_Loaded)
        LoadFromPreferences();
    return s_AccentFocusOutline;
}

void EditorSearchBars::SetAccentFocusOutline(bool enabled)
{
    s_AccentFocusOutline = enabled;
    s_Loaded = true;
    SaveToPreferences();

    for (UIElement* target : s_FocusTargets)
    {
        ApplyFocusOutline(target);
        if (target)
            target->MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
    }
}

void EditorSearchBars::SetAtTop(bool atTop)
{
    s_AtTop = atTop;
    s_Loaded = true;
    SaveToPreferences();
    PruneNulls();

    std::vector<UIManager*> managersToRefresh;
    for (UIElement* bar : s_Bars)
    {
        ApplyPlacement(bar);

        UIElement* dirtyRoot = bar->GetParent() ? bar->GetParent() : bar;
        dirtyRoot->MarkDirtySubtree(
            UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);

        UIManager* manager = bar->GetOwnerManager();
        if (manager && std::find(managersToRefresh.begin(), managersToRefresh.end(), manager) ==
                           managersToRefresh.end())
            managersToRefresh.push_back(manager);
    }

    for (UIManager* manager : managersToRefresh)
        manager->RequestRelayout();
}

void EditorSearchBars::Register(UIElement* searchBar)
{
    if (!searchBar)
        return;

    if (!s_Loaded)
        LoadFromPreferences();

    ApplyPlacement(searchBar);
    RegisterFocusTarget(searchBar);

    if (std::find(s_Bars.begin(), s_Bars.end(), searchBar) == s_Bars.end())
        s_Bars.push_back(searchBar);
}

void EditorSearchBars::RegisterFocusTarget(UIElement* searchBar)
{
    if (!searchBar)
        return;
    if (!s_Loaded)
        LoadFromPreferences();
    ApplyFocusOutline(searchBar);
    if (std::find(s_FocusTargets.begin(), s_FocusTargets.end(), searchBar) == s_FocusTargets.end())
        s_FocusTargets.push_back(searchBar);
}

void EditorSearchBars::UnregisterFocusTarget(UIElement* searchBar)
{
    auto it = std::remove(s_FocusTargets.begin(), s_FocusTargets.end(), searchBar);
    if (it != s_FocusTargets.end())
        s_FocusTargets.erase(it, s_FocusTargets.end());
}

void EditorSearchBars::ApplyFocusOutline(UIElement* searchBar)
{
    if (!searchBar)
        return;
    if (s_AccentFocusOutline)
        searchBar->AddClass("search-accent-focus");
    else
        searchBar->RemoveClass("search-accent-focus");
}

void EditorSearchBars::ApplyPlacement(UIElement* searchBar)
{
    if (!searchBar)
        return;

    if (s_AtTop)
        searchBar->AddClass("search-bar-at-top");
    else
        searchBar->RemoveClass("search-bar-at-top");

    UIElement* parent = searchBar->GetParent();
    if (!parent)
        return;

    if (s_AtTop)
        parent->AddClass("search-bars-at-top");
    else
        parent->RemoveClass("search-bars-at-top");
}

void EditorSearchBars::Unregister(UIElement* searchBar)
{
    if (!searchBar)
        return;

    auto it = std::remove(s_Bars.begin(), s_Bars.end(), searchBar);
    if (it != s_Bars.end())
        s_Bars.erase(it, s_Bars.end());
    UnregisterFocusTarget(searchBar);
}

void EditorSearchBars::PruneNulls()
{
    auto it = std::remove(s_Bars.begin(), s_Bars.end(), nullptr);
    if (it != s_Bars.end())
        s_Bars.erase(it, s_Bars.end());
}

void EditorSearchBars::LoadFromPreferences()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    // A key the profile never saved reads its default, not the value an earlier load left behind.
    s_AtTop = kDefaultAtTop;
    s_AccentFocusOutline = kDefaultAccentFocusOutline;
    prefs.TryGetBool("ui.searchBarsAtTop", s_AtTop);
    prefs.TryGetBool("ui.searchBarsAccentFocusOutline", s_AccentFocusOutline);
    s_Loaded = true;
}

void EditorSearchBars::SaveToPreferences()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err); // Load existing to preserve other settings
    
    prefs.SetBool("ui.searchBarsAtTop", s_AtTop);
    prefs.SetBool("ui.searchBarsAccentFocusOutline", s_AccentFocusOutline);
    prefs.Save(&err);
}

} // namespace GameEngine
