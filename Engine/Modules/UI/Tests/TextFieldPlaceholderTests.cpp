// A text field's placeholder: the hint text the field shows while it holds no text. The field draws
// it itself, as its own label child, so every field that wants a hint shows the same one; it hides
// the moment the field holds text and comes back when the text is cleared.

#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextField.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"

#include <memory>
#include <string>

using namespace GameEngine;
using namespace GameEngine::UIParsing;

namespace
{

constexpr char kPlaceholderClass[] = "field-placeholder";
constexpr char kHiddenClass[] = "hidden";

Label* FindPlaceholder(UIElement& field)
{
    for (const auto& child : field.GetChildren())
    {
        if (child && child->HasClass(kPlaceholderClass))
            return dynamic_cast<Label*>(child.get());
    }
    return nullptr;
}

bool IsShown(const Label& placeholder)
{
    return !placeholder.HasClass(kHiddenClass);
}

std::unique_ptr<UIElement> ParseLayout(const char* xml)
{
    UIRegistration::RegisterBuiltInControls();
    std::unique_ptr<UIElement> root;
    if (!XMLParser::ParseLayoutFromString(xml, root))
        return nullptr;
    return root;
}

} // namespace

TEST(TextFieldPlaceholderTests, AttributeShowsTheHintUntilTheFieldHoldsText)
{
    const auto root = ParseLayout(R"(<uielement><TextField id="query" placeholder="Find assets" /></uielement>)");
    ASSERT_NE(root, nullptr);
    auto* field = dynamic_cast<TextField*>(root->FindById("query"_sid));
    ASSERT_NE(field, nullptr);

    Label* placeholder = FindPlaceholder(*field);
    ASSERT_NE(placeholder, nullptr) << "the placeholder attribute gives the field a hint label";
    EXPECT_EQ(placeholder->GetText(), "Find assets");
    EXPECT_TRUE(IsShown(*placeholder)) << "an empty field shows its hint";

    ASSERT_TRUE(field->OnChar('a'));
    EXPECT_EQ(field->GetValue(), "a");
    EXPECT_FALSE(IsShown(*placeholder)) << "one typed character hides the hint";

    ASSERT_TRUE(field->OnKey(Input::kKeyCode_Backspace, 0, nullptr));
    EXPECT_EQ(field->GetValue(), "");
    EXPECT_TRUE(IsShown(*placeholder)) << "clearing the text brings the hint back";

    field->SetValue("set by the host");
    EXPECT_FALSE(IsShown(*placeholder)) << "text the host sets hides the hint";
    field->SetValueWithoutNotify("");
    EXPECT_TRUE(IsShown(*placeholder));
}

TEST(TextFieldPlaceholderTests, SearchFieldWithFilterShowsItsHintOnItsField)
{
    SearchFieldWithFilter search;
    search.SetPlaceholder("Search");
    ASSERT_NE(search.GetField(), nullptr);

    Label* placeholder = FindPlaceholder(*search.GetField());
    ASSERT_NE(placeholder, nullptr) << "the search field's hint is its text field's placeholder";
    EXPECT_EQ(placeholder->GetText(), "Search");
    EXPECT_TRUE(IsShown(*placeholder));

    search.SetValue("rock");
    EXPECT_FALSE(IsShown(*placeholder));
    search.SetValue("");
    EXPECT_TRUE(IsShown(*placeholder));
}

TEST(TextFieldPlaceholderTests, SetPlaceholderNamesTheHintAndEmptyHidesIt)
{
    TextField field;
    EXPECT_EQ(FindPlaceholder(field), nullptr) << "a field without a hint creates no label";

    field.SetPlaceholder("API key");
    Label* placeholder = FindPlaceholder(field);
    ASSERT_NE(placeholder, nullptr);
    EXPECT_EQ(placeholder->GetText(), "API key");
    EXPECT_TRUE(IsShown(*placeholder));
    EXPECT_FALSE(placeholder->IsFocusable()) << "the hint never takes focus";

    field.SetPlaceholder("");
    EXPECT_EQ(placeholder->GetText(), "");
    EXPECT_FALSE(IsShown(*placeholder)) << "an empty hint shows nothing";
}
