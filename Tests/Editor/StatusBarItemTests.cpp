// The status bar's registered items and the first of them, "Preparing N textures": an item
// shows its owner's text in its own label, in registration order, only while the text is
// non-empty, and the bar polls the texts a few times a second rather than every frame.

#include "Assets/TexturePreparingStatusItem.h"
#include "Editor/Registries/StatusBarItemRegistry.h"

#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

using GameEngine::Label;
using GameEngine::UIElement;
using GameEngine::Editor::MakeTexturePreparingStatusItem;
using GameEngine::Editor::StatusBarItemRegistry;
using GameEngine::Editor::TexturePreparingCounts;
using GameEngine::Editor::TexturePreparingStatusDisplay;

namespace
{

// The layout's bottom bar as far as the registry reads it: a root holding the item container.
std::unique_ptr<UIElement> MakeBar()
{
    auto root = std::make_unique<UIElement>();
    auto container = std::make_unique<UIElement>();
    container->SetId("StatusBarItems");
    root->AddChild(std::move(container));
    return root;
}

Label* ItemLabel(UIElement& root, const std::string& itemId)
{
    return dynamic_cast<Label*>(root.FindById(GameEngine::HashStringId("StatusBarItem." + itemId)));
}

bool Shows(const Label* label)
{
    return label && !label->HasClass("hidden") && !label->GetText().empty();
}

} // namespace

TEST(TexturePreparingStatus, ReadsTheLargerCountAndNothingWhenBothAreZero)
{
    EXPECT_EQ(TexturePreparingStatusDisplay({0, 0}).Text, "");
    EXPECT_EQ(TexturePreparingStatusDisplay({1, 0}).Text, "Preparing 1 texture");
    EXPECT_EQ(TexturePreparingStatusDisplay({0, 1}).Text, "Preparing 1 texture");
    EXPECT_EQ(TexturePreparingStatusDisplay({12, 67}).Text, "Preparing 67 textures");
    EXPECT_EQ(TexturePreparingStatusDisplay({9, 0}).Text, "Preparing 9 textures")
        << "a cook no material waits on yet still counts";
}

TEST(TexturePreparingStatus, TheTooltipSaysTheItemClearsWhenDone)
{
    EXPECT_EQ(TexturePreparingStatusDisplay({0, 0}).Tooltip, "");
    EXPECT_EQ(TexturePreparingStatusDisplay({0, 1}).Tooltip, "1 texture still preparing; clears when done");
    EXPECT_EQ(TexturePreparingStatusDisplay({12, 67}).Tooltip, "67 textures still preparing; clears when done");
}

TEST(TexturePreparingStatus, TheBarLabelFollowsTheCountsAtThePollInterval)
{
    TexturePreparingCounts counts{};
    StatusBarItemRegistry registry;
    registry.Register(MakeTexturePreparingStatusItem([&counts]() { return counts; }));
    auto bar = MakeBar();
    using std::chrono::milliseconds;

    registry.Update(milliseconds(0), bar.get());
    Label* label = ItemLabel(*bar, "TexturesPreparing");
    ASSERT_NE(label, nullptr) << "the first update creates the item's label";
    EXPECT_FALSE(Shows(label)) << "nothing is preparing";

    counts = {12, 67};
    registry.Update(StatusBarItemRegistry::kPollInterval, bar.get());
    EXPECT_TRUE(Shows(label));
    EXPECT_EQ(label->GetText(), "Preparing 67 textures");
    EXPECT_EQ(label->GetTooltip(), "67 textures still preparing; clears when done");

    counts = {0, 1};
    registry.Update(milliseconds(100), bar.get());
    EXPECT_EQ(label->GetText(), "Preparing 67 textures") << "polled before the interval passed";
    registry.Update(StatusBarItemRegistry::kPollInterval - milliseconds(100), bar.get());
    EXPECT_EQ(label->GetText(), "Preparing 1 texture");

    counts = {0, 0};
    registry.Update(StatusBarItemRegistry::kPollInterval, bar.get());
    EXPECT_FALSE(Shows(label)) << "the item clears once no texture cooks and no material waits";
}

TEST(StatusBarItemRegistry, ItemsShowInRegistrationOrderOnlyWhileTheyHaveText)
{
    std::string first = "first";
    std::string second;
    StatusBarItemRegistry registry;
    registry.Register({"First", [&first]() { return GameEngine::Editor::StatusBarItemDisplay{first, {}}; }});
    registry.Register({"Second", [&second]() { return GameEngine::Editor::StatusBarItemDisplay{second, {}}; }});
    auto bar = MakeBar();

    registry.Update(std::chrono::milliseconds(0), bar.get());
    const auto& labels = bar->FindById(GameEngine::HashStringId("StatusBarItems"))->GetChildren();
    ASSERT_EQ(labels.size(), 2u);
    EXPECT_EQ(labels[0].get(), ItemLabel(*bar, "First"));
    EXPECT_EQ(labels[1].get(), ItemLabel(*bar, "Second"));
    EXPECT_TRUE(Shows(ItemLabel(*bar, "First")));
    EXPECT_FALSE(Shows(ItemLabel(*bar, "Second")));

    first.clear();
    second = "second";
    registry.Update(StatusBarItemRegistry::kPollInterval, bar.get());
    EXPECT_FALSE(Shows(ItemLabel(*bar, "First")));
    EXPECT_TRUE(Shows(ItemLabel(*bar, "Second")));
    EXPECT_EQ(labels.size(), 2u) << "a poll reuses the labels it created";

    // A layout rebuild replaces the container; the next poll fills the new one.
    auto rebuilt = MakeBar();
    registry.Update(StatusBarItemRegistry::kPollInterval, rebuilt.get());
    EXPECT_TRUE(Shows(ItemLabel(*rebuilt, "Second")));
}

// Two items under one id would share one label and one item's text would never show, so a
// second registration of an id asserts.
#if !defined(NDEBUG)
TEST(StatusBarItemRegistryDeathTest, ASecondItemUnderOneIdAsserts)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH(
        {
            StatusBarItemRegistry registry;
            registry.Register({"Same", []() { return GameEngine::Editor::StatusBarItemDisplay{"first", {}}; }});
            registry.Register({"Same", []() { return GameEngine::Editor::StatusBarItemDisplay{"second", {}}; }});
        },
        "registered once");
}
#endif
