#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextField.h"

namespace GameEngine {

struct PanelSearchBarBuilt {
    std::unique_ptr<UIElement> Root;
    SearchFieldWithFilter* RootPtr = nullptr;
    UIElement* IconPtr = nullptr;
    Button* ClearButtonPtr = nullptr;
    TextField* FieldPtr = nullptr;
    Dropdown* FilterPtr = nullptr;
};

// Builds the standard editor panel search bar: a `.panel-search-bar` field with the "Search"
// placeholder, the search icon, the clear action and an optional filter dropdown. The field
// reads as a field at rest (theme/widgets.css); focus and query styling come from
// SearchFieldWithFilter.
//
// `fieldId` names the TextField for automation (debug-server input_text /
// click_element target by element id) — unique per window, e.g.
// "hierarchy-search-field".
//
// The caller is responsible for:
// - Adding the returned `root` into the UI tree
// - Registering/unregistering the returned `rootPtr` with SettingsPanel::RegisterSearchBar/UnregisterSearchBar
inline PanelSearchBarBuilt BuildPanelSearchBar(
    const std::string& fieldId,
    std::function<void(const std::string&)> onTextChanged = {},
    std::function<void(const std::string&)> onTextChanging = {},
    const std::vector<Dropdown::Option>& filterOptions = {},
    std::function<void(const std::string&)> onFilterChanged = {})
{
    PanelSearchBarBuilt out{};

    auto searchBar = std::make_unique<SearchFieldWithFilter>();
    searchBar->AddClass("panel-search-bar");

    SearchFieldWithFilter* searchBarPtr = searchBar.get();
    searchBar->SetFieldId(fieldId);
    searchBar->SetPlaceholder("Search");
    searchBar->SetValue("");
    searchBar->SetFilterOptions(filterOptions, 0);
    searchBar->SetOnFilterChanged(std::move(onFilterChanged));

    TextField* fieldPtr = searchBar->GetField();
    UIElement* iconPtr = searchBar->GetSearchIcon();
    Dropdown* filterPtr = filterOptions.empty() ? nullptr : searchBar->GetFilter();
    fieldPtr->AddClass("panel-search-field");
    iconPtr->AddClass("panel-search-icon");
    searchBar->GetFilter()->AddClass("panel-search-filter");
    searchBar->GetClearButton()->AddClass("panel-search-clear");

    if (onTextChanging)
        searchBar->SetOnQueryChanging(std::move(onTextChanging));
    else
        searchBar->SetOnQueryChanging(onTextChanged);
    searchBar->SetOnQueryChanged(std::move(onTextChanged));

    out.RootPtr = searchBarPtr;
    out.IconPtr = iconPtr;
    out.ClearButtonPtr = searchBarPtr->GetClearButton();
    out.FieldPtr = fieldPtr;
    out.FilterPtr = filterPtr;
    out.Root = std::move(searchBar);
    return out;
}

} // namespace GameEngine
