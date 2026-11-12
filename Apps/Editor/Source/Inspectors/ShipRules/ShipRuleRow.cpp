#include "Inspectors/ShipRules/ShipRuleRow.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/ShipRules/SetShipRuleCommand.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

constexpr const char* kLayoutAssetPath = "UI/inspectors/ShipRuleRow/ShipRuleRow.uxml";
constexpr std::string_view kLabelId = "ship-rule-label";
constexpr std::string_view kDropdownId = "ship-rule-dropdown";
constexpr std::string_view kCaptionRowId = "ship-rule-caption-row";
constexpr std::string_view kCaptionId = "ship-rule-caption";
constexpr std::string_view kClearRowId = "ship-rule-clear-row";
constexpr std::string_view kClearId = "ship-rule-clear";
constexpr std::array<std::string_view, 6> kRequiredIds = {kLabelId, kDropdownId, kCaptionRowId,
                                                          kCaptionId, kClearRowId, kClearId};

constexpr const char* kRowTooltip =
    "Inferred: ships when a scene or another shipped asset uses it.\n"
    "Always: ships on every platform, even if nothing uses it. Use it for files the game opens by path.\n"
    "Never: never ships. The build stops if a shipped asset uses it.\n"
    "A folder's rule applies to everything in it; a rule set on an asset or a subfolder wins.";

// Sets `rule` on every selected file and folder, or clears their own rules, as one undo step.
void ApplyShipRule(const InspectorContext& ctx, std::optional<ShipRule> rule)
{
    auto command = std::make_unique<SetShipRuleCommand>(ctx.AssetPaths, rule, ctx.RequestInspectorRefresh);
    if (ctx.Undo)
        ctx.Undo->Execute(std::move(command));
    else
        command->Do();
}

void AddShipRuleRow(const InspectorContext& ctx)
{
    ShipRuleSelection selection;
    if (!ShipRuleSelection::TryRead(EngineCore::GetInstance().GetAssetManager(),
                                    EngineCore::GetInstance().GetResolvedAssetRoot(), ctx.AssetPaths, selection))
        return;
    InspectorUI::AddControl(ctx.Parent, std::make_unique<ShipRuleRow>(selection.Describe(),
                                                                      [ctx](std::optional<ShipRule> rule)
                                                                      { ApplyShipRule(ctx, rule); }));
}

} // namespace

void ShipRuleRow::Register()
{
    InspectorRegistry::Get().RegisterAssetSelectionRow("ship-in-builds", &AddShipRuleRow);
}

ShipRuleRow::ShipRuleRow(const ShipRuleSelection::State& state, std::function<void(std::optional<ShipRule>)> onPick)
{
    std::unique_ptr<UIElement> body = ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!body)
        return;
    auto* label = dynamic_cast<Label*>(body->FindById(HashStringId(kLabelId)));
    auto* dropdown = dynamic_cast<Dropdown*>(body->FindById(HashStringId(kDropdownId)));
    auto* caption = dynamic_cast<Label*>(body->FindById(HashStringId(kCaptionId)));
    auto* clear = dynamic_cast<Button*>(body->FindById(HashStringId(kClearId)));
    if (!label || !dropdown || !caption || !clear)
    {
        Logger::Log::Error("ShipRuleRow: {} needs a Label '{}', a Dropdown '{}', a Label '{}' and a Button '{}'.",
                           kLayoutAssetPath, kLabelId, kDropdownId, kCaptionId, kClearId);
        return;
    }

    label->SetTooltip(kRowTooltip);
    std::vector<Dropdown::Option> options;
    for (ShipRule choice : ShipRuleSelection::kChoices)
        options.push_back({std::string(ShipRules::ToText(choice)), ShipRuleSelection::Label(choice)});
    dropdown->SetOptions(options, state.SelectedIndex);
    dropdown->SetNoSelectionText(state.NoSelectionText);
    dropdown->SetSelectedIndexWithoutNotify(state.SelectedIndex);
    InspectorUI::SetRowOverridden(InspectorUI::RowOfControl(dropdown), state.AnyOwn);
    if (state.NeverShips)
        InspectorUI::DisableRowOfControl(dropdown, "Not game content: it never ships.");
    dropdown->SetOnValueChanged([onPick](const std::string& value)
                                { onPick(ShipRules::Parse(value)); });

    caption->SetText(state.Caption);
    if (state.Caption.empty())
        body->RemoveChild(body->FindById(HashStringId(kCaptionRowId)));
    clear->SetTooltip("Removes the rule set here, so the folder's rule or the default applies.");
    clear->SetOnClick([onPick](UIEvent&)
                      { onPick(std::nullopt); });
    if (!state.AnyOwn)
        body->RemoveChild(body->FindById(HashStringId(kClearRowId)));
    AddChild(std::move(body));
}

} // namespace GameEngine::Editor
