#pragma once

#include "Editor/Settings/EditorSettingsRegistry.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace GameEngine
{
class Label;
class TextField;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{

// What a settings text or path row is built from: the StringField or PathField descriptor's values.
struct SettingsTextFieldRowConfig
{
    std::string LabelText;
    std::string Tooltip;
    std::string DefaultValue;
    // Editor preferences key the row loads from and saves to; empty when Get and Set own the value.
    std::string PrefKey;
    std::function<std::string()> Get;
    std::function<void(const std::string&)> Set;
    // The refusal message for a value, empty when it is accepted (StringField::Validate).
    std::function<std::string(const std::string&)> Validate;
    // Shown faint in the field while the value is empty.
    std::string Placeholder;
    // When set, a Browse button opens the native picker of this kind and writes the chosen path into
    // the field.
    std::optional<SettingsFieldDescriptor::PathField::Kind> BrowseKind;
};

// The parts of a built row. The page adds Row, then Refusal under it when there is one.
struct SettingsTextFieldRow
{
    std::unique_ptr<UIElement> Row;
    // The line under the row that says why a typed value is refused; null without a Validate hook.
    std::unique_ptr<Label> Refusal;
    Label* RowLabel = nullptr;
    TextField* Field = nullptr;
    // Puts the default value back, through the same commit as an edit (the label's double-click).
    std::function<void()> Reset;
};

// Builds a settings text row. One commit path serves Enter, focus-out, Browse and Reset, so a value never
// reaches Set without reaching the store. A refused value shows its message under the row while it is
// typed; a refused commit never reaches Set and the field goes back to the stored value with the message
// still shown. The message clears when the user types an accepted value or the stored value comes back
// (Escape, focus-out).
SettingsTextFieldRow CreateSettingsTextFieldRow(const SettingsTextFieldRowConfig& config);

} // namespace GameEngine::Editor
