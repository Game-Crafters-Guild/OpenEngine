#include "Editor/Settings/SettingsTextFieldRow.h"

#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Platform/Shell.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"

#include <filesystem>
#include <system_error>

namespace GameEngine::Editor
{
namespace
{
// Shows `placeholder` faint after the text while `value` is empty, and nothing otherwise.
void ShowPlaceholder(TextField* field, const std::string& placeholder, const std::string& value)
{
    if (!placeholder.empty())
        field->SetSuffix(value.empty() ? placeholder : std::string());
}

// Shows a refusal message under the row, or hides it when `message` is empty.
void ShowRefusal(Label* refusal, const std::string& message)
{
    if (!refusal)
        return;
    refusal->SetText(message);
    if (message.empty())
        refusal->AddClass("hidden");
    else
        refusal->RemoveClass("hidden");
}

std::string InitialValue(const SettingsTextFieldRowConfig& config)
{
    std::string value = config.DefaultValue;
    if (config.Get)
        return config.Get();
    if (!config.PrefKey.empty())
    {
        auto prefs = OpenEditorPreferences();
        std::string error;
        prefs.Load(&error);
        prefs.TryGetString(config.PrefKey, value);
    }
    return value;
}

void SavePreference(const std::string& prefKey, const std::string& value)
{
    if (prefKey.empty())
        return;
    auto prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    prefs.SetString(prefKey, value);
    prefs.Save(&error);
}

// Seeds the native picker from the field's value when it names a real location, else from the open
// project, and returns the chosen path (empty when cancelled).
std::string BrowseFor(SettingsFieldDescriptor::PathField::Kind kind, const std::string& current)
{
    std::filesystem::path initial = EngineCore::GetInstance().GetWorkspaceRoot();
    const std::filesystem::path currentPath(current);
    std::error_code ec;
    if (!currentPath.empty() && std::filesystem::exists(currentPath, ec))
        initial = currentPath;
    const std::filesystem::path picked = kind == SettingsFieldDescriptor::PathField::Kind::Directory
                                             ? Platform::SelectFolder(initial)
                                             : Platform::SelectFile(initial);
    return picked.string();
}
} // namespace

SettingsTextFieldRow CreateSettingsTextFieldRow(const SettingsTextFieldRowConfig& config)
{
    SettingsTextFieldRow result;
    result.Row = std::make_unique<UIElement>();
    result.Row->AddClass("settings-row");

    auto label = std::make_unique<Label>();
    label->SetText(config.LabelText);
    if (!config.Tooltip.empty())
        label->SetTooltip(config.Tooltip);
    label->AddClass("settings-row-label");
    result.RowLabel = label.get();
    result.Row->AddChild(std::move(label));

    const std::string currentValue = InitialValue(config);
    auto field = std::make_unique<TextField>();
    field->SetValue(currentValue);
    if (!config.Tooltip.empty())
        field->SetTooltip(config.Tooltip);
    field->AddClass("settings-row-field");
    TextField* fieldPtr = field.get();
    result.Field = fieldPtr;
    if (!config.Placeholder.empty())
    {
        fieldPtr->SetSuffixAlignment(SuffixAlignment::AfterText);
        ShowPlaceholder(fieldPtr, config.Placeholder, currentValue);
    }

    Label* refusalPtr = nullptr;
    if (config.Validate)
    {
        result.Refusal = std::make_unique<Label>();
        result.Refusal->AddClass("settings-row-refusal");
        result.Refusal->AddClass("hidden");
        refusalPtr = result.Refusal.get();
    }

    // Focus-out fires whether or not the user typed anything, so an unchanged value must not write:
    // tabbing through a page is not an edit, and writing there would persist the row's own default and
    // stop that key tracking the default from then on. Same obligation the toggle and slider rows carry.
    auto committed = std::make_shared<std::string>(currentValue);
    auto commit = [config, committed, fieldPtr, refusalPtr](const std::string& value)
    {
        if (*committed == value)
        {
            // The stored value is back (Escape, a focus-out after a refusal): nothing is refused.
            ShowRefusal(refusalPtr, std::string());
            ShowPlaceholder(fieldPtr, config.Placeholder, value);
            return;
        }
        if (config.Validate)
        {
            const std::string message = config.Validate(value);
            ShowRefusal(refusalPtr, message);
            if (!message.empty())
            {
                fieldPtr->SetValueWithoutNotify(*committed);
                ShowPlaceholder(fieldPtr, config.Placeholder, *committed);
                return;
            }
        }
        *committed = value;
        ShowPlaceholder(fieldPtr, config.Placeholder, value);
        if (config.Set)
            config.Set(value);
        SavePreference(config.PrefKey, value);
    };

    fieldPtr->SetOnValueChanged(commit);
    if (config.Validate || !config.Placeholder.empty())
    {
        fieldPtr->SetOnValueChanging([config, fieldPtr, refusalPtr](const std::string& value)
        {
            if (config.Validate)
                ShowRefusal(refusalPtr, config.Validate(value));
            ShowPlaceholder(fieldPtr, config.Placeholder, value);
        });
    }
    fieldPtr->RegisterEventHandler(kEventFocusOut, [fieldPtr, commit](UIEvent&) { commit(fieldPtr->GetValue()); });
    result.Row->AddChild(std::move(field));

    if (config.BrowseKind.has_value())
    {
        const SettingsFieldDescriptor::PathField::Kind kind = *config.BrowseKind;
        auto browse = std::make_unique<Button>();
        browse->SetText("Browse...");
        browse->AddClass("small");
        browse->AddClass("secondary");
        browse->SetTooltip(kind == SettingsFieldDescriptor::PathField::Kind::Directory ? "Choose a folder."
                                                                                     : "Choose a file.");
        browse->RegisterEventHandler(kEventButtonClick, [fieldPtr, commit, kind](UIEvent&)
        {
            const std::string picked = BrowseFor(kind, fieldPtr->GetValue());
            if (picked.empty())
                return; // cancelled
            fieldPtr->SetValue(picked);
            commit(picked);
        });
        result.Row->AddChild(std::move(browse));
    }

    result.Reset = [fieldPtr, commit, defaultValue = config.DefaultValue]()
    {
        fieldPtr->SetValue(defaultValue);
        commit(defaultValue);
    };
    return result;
}

} // namespace GameEngine::Editor
