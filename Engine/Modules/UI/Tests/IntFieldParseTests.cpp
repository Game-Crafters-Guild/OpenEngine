// An IntField stores a typed whole number exactly, and clamps a typed number past the range of int.

#include "Input/KeyCodes.h"
#include "UI/Controls/IntField.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using namespace GameEngine;

namespace
{
TextInput* EditorOf(IntField& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (auto* editor = dynamic_cast<TextInput*>(child.get()))
            return editor;
    }
    return nullptr;
}

int TypeAndCommit(IntField& field, const std::string& text)
{
    TextInput* editor = EditorOf(field);
    EXPECT_NE(editor, nullptr);
    if (editor == nullptr)
        return 0;
    field.OnFocusChanged(true);
    editor->SetValue("");
    for (const char c : text)
        editor->OnChar(static_cast<unsigned char>(c));
    editor->OnKey(Input::kKeyCode_Enter, 0, nullptr);
    field.OnFocusChanged(false);
    return field.GetValue();
}
} // namespace

TEST(IntFieldParse, ATypedWholeNumberIsStoredExactly)
{
    IntField field;
    EXPECT_EQ(TypeAndCommit(field, "16777217"), 16777217);
    EXPECT_EQ(TypeAndCommit(field, "123456789"), 123456789);
    EXPECT_EQ(TypeAndCommit(field, "2147483647"), 2147483647);
    EXPECT_EQ(TypeAndCommit(field, "-2147483648"), std::numeric_limits<int>::min());
}

TEST(IntFieldParse, ATypedNumberPastTheRangeOfIntIsClamped)
{
    IntField field;
    EXPECT_EQ(TypeAndCommit(field, "99999999999"), std::numeric_limits<int>::max());
    EXPECT_EQ(TypeAndCommit(field, "-99999999999"), std::numeric_limits<int>::min());
    EXPECT_EQ(TypeAndCommit(field, "1e12"), std::numeric_limits<int>::max());
}
