#pragma once

#include "Inspectors/ShipRules/ShipRuleSelection.h"
#include "UI/UIElement.h"

#include <functional>
#include <optional>

namespace GameEngine::Editor
{

/// The Inspector's Ship in builds row for project files and folders: the rule dropdown, the line under it
/// and Clear rule. The layout is UI/inspectors/ShipRuleRow/ShipRuleRow.uxml.
class ShipRuleRow final : public UIElement
{
  public:
    /// Registers the row above the view of every asset selection (InspectorRegistry::RegisterAssetSelectionRow).
    static void Register();

    /// Shows `state`. `onPick` gets the rule picked in the dropdown, or nullopt from Clear rule.
    ShipRuleRow(const ShipRuleSelection::State& state, std::function<void(std::optional<ShipRule>)> onPick);
};

} // namespace GameEngine::Editor
