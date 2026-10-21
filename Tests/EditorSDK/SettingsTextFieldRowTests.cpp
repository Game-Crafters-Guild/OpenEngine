// A settings text row with a Validate hook, driven the way a user types: a refused value shows its message
// under the row while typed, Enter keeps it from Set and puts the stored value back, an accepted value hides
// the message, and Escape after a refused edit clears it.

#include "Editor/Settings/SettingsTextFieldRow.h"

#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
constexpr int kControlModifier = 0x0002;

TextInput* EditorOf(TextField& field)
{
    for (const auto& child : field.GetChildren())
        if (auto* input = dynamic_cast<TextInput*>(child.get()))
            return input;
    return nullptr;
}

// Replaces the field's text with `text`, keystroke by keystroke, without committing.
void Type(TextField& field, const std::string& text)
{
    TextInput* editor = EditorOf(field);
    ASSERT_NE(editor, nullptr);
    field.OnFocusChanged(true);
    editor->OnKey(Input::kKeyCode_A, kControlModifier, nullptr);
    if (text.empty())
        editor->OnKey(Input::kKeyCode_Backspace, 0, nullptr);
    for (const char c : text)
        editor->OnChar(static_cast<unsigned char>(c));
}

void Press(TextField& field, int key)
{
    EditorOf(field)->OnKey(key, 0, nullptr);
}

bool Shown(const Label& refusal)
{
    return !refusal.HasClass("hidden");
}

struct Row
{
    std::string Stored = "kept";
    std::vector<std::string> Sets;
    Editor::SettingsTextFieldRow Built;
};

void Build(Row& row)
{
    Editor::SettingsTextFieldRowConfig config;
    config.LabelText = "Name";
    config.Get = [&row]() { return row.Stored; };
    config.Set = [&row](const std::string& value) { row.Sets.push_back(value); row.Stored = value; };
    config.Validate = [](const std::string& value)
    { return value == "taken" ? std::string("'taken' is already in use.") : std::string(); };
    row.Built = Editor::CreateSettingsTextFieldRow(config);
}
} // namespace

TEST(SettingsTextFieldRowTest, ARefusedValueShowsItsMessageAndNeverReachesSet)
{
    Row row;
    Build(row);
    ASSERT_NE(row.Built.Refusal, nullptr);
    TextField& field = *row.Built.Field;
    Label& refusal = *row.Built.Refusal;
    EXPECT_FALSE(Shown(refusal));

    Type(field, "taken");
    EXPECT_TRUE(Shown(refusal)) << "the message shows while the refused value is typed";
    EXPECT_EQ(refusal.GetText(), "'taken' is already in use.");

    Press(field, Input::kKeyCode_Enter);
    EXPECT_TRUE(row.Sets.empty()) << "a refused commit never reaches Set";
    EXPECT_EQ(field.GetValue(), "kept") << "the field goes back to the stored value";
    EXPECT_TRUE(Shown(refusal)) << "the message says why it went back";

    Type(field, "free");
    EXPECT_FALSE(Shown(refusal)) << "an accepted value hides the message";
    Press(field, Input::kKeyCode_Enter);
    ASSERT_EQ(row.Sets.size(), 1u);
    EXPECT_EQ(row.Sets[0], "free");
}

TEST(SettingsTextFieldRowTest, EscapeAfterARefusedEditClearsTheMessage)
{
    Row row;
    Build(row);
    TextField& field = *row.Built.Field;
    Label& refusal = *row.Built.Refusal;

    Type(field, "taken");
    ASSERT_TRUE(Shown(refusal));
    Press(field, Input::kKeyCode_Escape);
    EXPECT_EQ(field.GetValue(), "kept");
    EXPECT_FALSE(Shown(refusal)) << "the stored value is back, so nothing is refused";
    EXPECT_TRUE(row.Sets.empty());
}
