#include "DayCycle/DayCycleProfileInspector.h"

#include "Assets/DayCycleProfile.h"
#include "Core/Engine.h"
#include "DayCycle/DayCycleAxis.h"
#include "DayCycle/DayCycleDescriptionField.h"
#include "DayCycle/DayCycleEditorServices.h"
#include "DayCycle/DayCycleProfileControls.h"
#include "DayCycle/DayCycleRowsHost.h"
#include "DayCycle/DayCycleSunNow.h"
#include "DayCycle/DayCycleTrackRows.h"
#include "DayCycle/DayCycleWindowRegistration.h"
#include "DayCycle/DayCycleWorkingCopy.h"
#include "Editor/Registries/EditorPanelRegistry.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"

#include <memory>

namespace GameEngine::Editor
{
namespace
{
void AddNameRow(UIElement& parent, const std::shared_ptr<DayCycleWorkingCopy>& profile, UndoRedoService* undo)
{
    TextField* field = InspectorUI::AddTextRow(&parent, "Name", profile->GetData().Name,
                                               "The profile's name, as the pickers and presets list it.");
    field->SetOnCommit([field, profile, undo]() {
        DayCycleProfileData data = profile->GetData();
        if (data.Name == field->GetValue())
            return;
        data.Name = field->GetValue();
        profile->Commit(data, "Rename the Day Cycle profile", undo);
    });
}

void CommitDescription(const std::shared_ptr<DayCycleWorkingCopy>& profile, UndoRedoService* undo,
                       const std::string& text)
{
    DayCycleProfileData data = profile->GetData();
    if (data.Description == text)
        return;
    data.Description = text;
    profile->Commit(data, "Edit the Day Cycle profile's description", undo);
}

void AddDescriptionRow(UIElement& parent, const std::shared_ptr<DayCycleWorkingCopy>& profile, UndoRedoService* undo)
{
    constexpr const char* kTooltip = "What the profile does to the day.";
    UIElement* row = InspectorUI::AddRow(&parent);
    InspectorUI::AddLabel(row, "Description", kTooltip);
    auto field = std::make_unique<DayCycleDescriptionField>(
        [profile, undo](const std::string& text) { CommitDescription(profile, undo, text); });
    field->SetValue(profile->GetData().Description);
    field->SetTooltip(kTooltip);
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(field));
}
} // namespace

void BuildDayCycleProfileInspector(const InspectorContext& context, const DayCycleEditorServices& services)
{
    const auto* asset = static_cast<const DayCycleProfile*>(context.Object);
    if (!context.Parent || !asset || !services.Assets)
        return;
    std::string error;
    const std::shared_ptr<DayCycleWorkingCopy> profile =
        DayCycleWorkingCopy::Open(*services.Assets, asset->GetPath(), error);
    if (!profile)
    {
        InspectorUI::AddTextBlock(context.Parent, "The profile could not be opened: " + error, "inspector-field-caption");
        return;
    }
    UIElement& parent = *context.Parent;
    parent.RequestSubtreeStyleAssetPath("UI/panels/DayCycle.css");
    UndoRedoService* undo = context.Undo ? context.Undo : services.Undo;

    // The profile's own header: Save (or Save a copy to the project) with the unsaved mark, at the
    // right, and the window that edits the same profile.
    UIElement* saveRow = InspectorUI::AddActionRow(&parent, "day-cycle-save-row");
    saveRow->AddClass("day-cycle-header-actions");
    auto openButton = std::make_unique<Button>();
    openButton->SetText("Open in the Day Cycle window");
    Button* open = openButton.get();
    saveRow->AddChild(std::move(openButton));
    open->SetOnClick([profile](UIEvent&) {
        DayCycleWorkingCopy::Show(profile);
        EditorPanelRegistry::Get().OpenPanel(kDayCycleWindowPanelId);
    });
    open->SetTooltip("The Day Cycle window edits this same profile: one undo history, one Save.");
    AddDayCycleSaveButton(*saveRow, profile, context.PingAsset).AddClass("day-cycle-save-trailing");

    AddNameRow(parent, profile, undo);
    AddDescriptionRow(parent, profile, undo);

    UIElement* axisRow = InspectorUI::AddRow(&parent);
    InspectorUI::AddLabel(axisRow, "Axis", "Which axis leads; the other is printed under it at every tick.");
    UIElement* axisField = InspectorUI::AddFieldContainer(axisRow);
    AddDayCycleAxisToggle(*axisField, DayCycleSessionAxisView(), [refresh = context.RequestInspectorRefresh](DayCycleAxisView view) {
        DayCycleSessionAxisView() = view;
        if (refresh)
            refresh();
    });
    InspectorUI::AddTextBlock(axisField, "The hours under the axis come from the open scene's sun path.",
                              "inspector-field-caption");

    DayCycleTrackRowsContext rowsContext;
    rowsContext.Profile = profile;
    rowsContext.Undo = undo;
    rowsContext.View = DayCycleSessionAxisView();
    if (ECS::World* world = services.World ? services.World() : nullptr)
        rowsContext.Path = ReadDayCycleSunNow(*world, nullptr, nullptr).Path;
    parent.AddChild(std::make_unique<DayCycleRowsHost>(std::move(rowsContext), services));

    AddDayCyclePresets(parent, *services.Assets, profile, undo);
}

void RegisterDayCycleProfileInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(AssetType::DayCycleProfile, [](const InspectorContext& context) {
        BuildDayCycleProfileInspector(context, EditorDayCycleServices(context.Undo, context.ChangeNotifications, {}, {}));
    });
}

} // namespace GameEngine::Editor
