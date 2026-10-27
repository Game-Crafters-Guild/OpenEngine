// UI::FocusIsInTextInput: is the focused element a text edit, optionally under a given element.
// A text field takes the focus for its inner text input; a text area takes it itself; an element
// inside a text field edits its text too. A container that only holds a text field does not.
#include <gtest/gtest.h>

#include "UI/Controls/FloatField.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/Interaction/FocusQueries.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>

using namespace GameEngine;

namespace
{

// root
//  +- panel (focusable)
//  |   +- name (TextField)
//  |   |   +- clear
//  |   +- scale (FloatField)
//  |   +- notes (TextArea)
//  +- other
//      +- button (focusable)
//      +- search (TextField)
struct FocusTree
{
    std::unique_ptr<UIElement> Root;
    UIElement* Panel = nullptr;
    UIElement* Other = nullptr;
};

template <typename T>
T* AddWithId(UIElement& parent, const char* id)
{
    auto element = std::make_unique<T>();
    element->SetId(id);
    element->SetFocusable(true);
    T* raw = element.get();
    parent.AddChild(std::move(element));
    return raw;
}

FocusTree BuildFocusTree()
{
    FocusTree tree;
    tree.Root = std::make_unique<UIElement>();
    tree.Root->SetId("root");
    tree.Panel = AddWithId<UIElement>(*tree.Root, "panel");
    TextField* name = AddWithId<TextField>(*tree.Panel, "name");
    AddWithId<UIElement>(*name, "clear");
    AddWithId<FloatField>(*tree.Panel, "scale");
    AddWithId<TextArea>(*tree.Panel, "notes");
    tree.Other = AddWithId<UIElement>(*tree.Root, "other");
    AddWithId<UIElement>(*tree.Other, "button");
    AddWithId<TextField>(*tree.Other, "search");
    return tree;
}

} // namespace

TEST(FocusIsInTextInput, NoFocusIsNoTextEdit)
{
    UIManager ui{nullptr};
    FocusTree tree = BuildFocusTree();
    ui.SetRoot(std::move(tree.Root));
    EXPECT_FALSE(UI::FocusIsInTextInput(ui));
}

TEST(FocusIsInTextInput, AFocusedTextFieldNumberFieldOrTextAreaEditsText)
{
    UIManager ui{nullptr};
    FocusTree tree = BuildFocusTree();
    ui.SetRoot(std::move(tree.Root));
    for (const char* id : {"name", "scale", "notes", "search"})
    {
        ui.SetFocusById(id);
        EXPECT_TRUE(UI::FocusIsInTextInput(ui)) << id;
    }
}

TEST(FocusIsInTextInput, AnElementInsideATextFieldEditsText)
{
    UIManager ui{nullptr};
    FocusTree tree = BuildFocusTree();
    ui.SetRoot(std::move(tree.Root));
    ui.SetFocusById("clear");
    EXPECT_TRUE(UI::FocusIsInTextInput(ui));
}

TEST(FocusIsInTextInput, AFocusedContainerOrButtonDoesNotEditText)
{
    UIManager ui{nullptr};
    FocusTree tree = BuildFocusTree();
    ui.SetRoot(std::move(tree.Root));
    ui.SetFocusById("panel");
    EXPECT_FALSE(UI::FocusIsInTextInput(ui)) << "the panel holds text fields but is not one";
    ui.SetFocusById("button");
    EXPECT_FALSE(UI::FocusIsInTextInput(ui));
}

TEST(FocusIsInTextInput, WithinAnElementTheFocusMustBeUnderIt)
{
    UIManager ui{nullptr};
    FocusTree tree = BuildFocusTree();
    UIElement* panel = tree.Panel;
    UIElement* other = tree.Other;
    ui.SetRoot(std::move(tree.Root));

    ui.SetFocusById("name");
    EXPECT_TRUE(UI::FocusIsInTextInput(ui, panel));
    EXPECT_FALSE(UI::FocusIsInTextInput(ui, other)) << "the focused field is under the panel, not under other";

    ui.SetFocusById("search");
    EXPECT_FALSE(UI::FocusIsInTextInput(ui, panel));
    EXPECT_TRUE(UI::FocusIsInTextInput(ui, other));

    ui.SetFocusById("button");
    EXPECT_FALSE(UI::FocusIsInTextInput(ui, other)) << "the focus is under other, but on a button";
}
