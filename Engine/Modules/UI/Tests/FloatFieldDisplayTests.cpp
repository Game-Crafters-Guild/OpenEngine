#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/TextField.h"

#include <limits>
#include <string>

using namespace GameEngine;

namespace {

constexpr float kLowest = std::numeric_limits<float>::lowest();

TextInput* FindEditor(FloatField& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

std::string ShownText(FloatField& field)
{
    TextInput* editor = FindEditor(field);
    return editor ? editor->GetValue() : std::string();
}

// Deletes the text with the keys a person uses.
void ClearText(TextInput& editor)
{
    editor.OnKey(Input::kKeyCode_End, 0, nullptr);
    for (int i = 0; i < 64 && !editor.GetValue().empty(); ++i)
        editor.OnKey(Input::kKeyCode_Backspace, 0, nullptr);
}

void Type(TextInput& editor, const std::string& text)
{
    for (const char c : text)
        editor.OnChar(static_cast<unsigned int>(c));
}

} // namespace

// An optional limit that is off is stored at the float limits. The field shows
// one word for it, never the 39 digits of the lowest float.
TEST(FloatFieldDisplayTests, ValuesAtTheFloatLimitsShowNone)
{
    for (const float value : {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()})
    {
        FloatField field;
        field.SetValueWithoutNotify(value);
        EXPECT_EQ(ShownText(field), "None") << value;
        EXPECT_EQ(field.GetValue(), value);
    }
}

TEST(FloatFieldDisplayTests, FixedDecimalPlacesAlsoShowNoneAtTheFloatLimits)
{
    FloatField field;
    field.SetFixedDecimalPlaces(2);
    field.SetValueWithoutNotify(std::numeric_limits<float>::lowest());
    EXPECT_EQ(ShownText(field), "None");
}

// Editing shows the exact value, so a focus and a blur never change it.
TEST(FloatFieldDisplayTests, EditingAnUnboundedValueShowsTheExactNumber)
{
    FloatField field;
    field.SetValueWithoutNotify(std::numeric_limits<float>::lowest());
    field.OnFocusChanged(true);
    float parsed = 0.0f;
    ASSERT_TRUE(FloatField::TryParseFloat(ShownText(field), parsed)) << ShownText(field);
    EXPECT_EQ(parsed, std::numeric_limits<float>::lowest());
    field.OnFocusChanged(false);
    EXPECT_EQ(ShownText(field), "None");
    EXPECT_EQ(field.GetValue(), std::numeric_limits<float>::lowest());
}

TEST(FloatFieldDisplayTests, LargeFiniteValuesStillShowANumber)
{
    FloatField field;
    field.SetValueWithoutNotify(1.0e30f);
    EXPECT_NE(ShownText(field), "None");
}

// A field with a None value goes back to None when the person clears the text.
TEST(FloatFieldDisplayTests, ClearingTheTextCommitsTheNoneValue)
{
    FloatField field;
    field.SetNoneValue(kLowest);
    field.SetValue(3.0f);
    field.OnFocusChanged(true);
    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    ClearText(*editor);
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), kLowest);
    field.OnFocusChanged(false);
    EXPECT_EQ(ShownText(field), "None");
}

TEST(FloatFieldDisplayTests, TypingTheWordNoneCommitsTheNoneValue)
{
    FloatField field;
    field.SetNoneValue(kLowest);
    field.SetValue(3.0f);
    field.OnFocusChanged(true);
    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    editor->SetValue("");
    Type(*editor, "none");
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), kLowest);
}

// The edit starts from empty text, not from the 39 digits of the lowest float.
TEST(FloatFieldDisplayTests, EditingANoneValueStartsFromEmptyText)
{
    FloatField field;
    field.SetNoneValue(kLowest);
    field.SetValueWithoutNotify(kLowest);
    field.OnFocusChanged(true);
    EXPECT_EQ(ShownText(field), "");
    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    Type(*editor, "5");
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), 5.0f);
}

// A field with no None value keeps its value when the person types the word.
TEST(FloatFieldDisplayTests, AFieldWithoutANoneValueRefusesTheWord)
{
    FloatField field;
    field.SetValue(3.0f);
    field.OnFocusChanged(true);
    TextInput* editor = FindEditor(field);
    ASSERT_NE(editor, nullptr);
    editor->SetValue("");
    Type(*editor, "None");
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    EXPECT_EQ(field.GetValue(), 3.0f);
}
