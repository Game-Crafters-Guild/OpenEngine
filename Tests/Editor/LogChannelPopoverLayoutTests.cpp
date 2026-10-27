// The Log panel's Channels list laid out over the editor's own stylesheets: the Level column lines up with its
// heading and stays in place whether the list scrolls or a search leaves it too short to scroll, and a hidden
// channel dims its name and its level alike.

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Logger/LogChannel.h"
#include "Logger/Logger.h"
#include "Panels/LogChannelPopover.h"
#include "Panels/LogView.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using GameEngine::Dropdown;
using GameEngine::LogChannelPopover;
using GameEngine::LogView;
using GameEngine::UIElement;
using InspectorLayoutTesting::FindByClass;
using InspectorLayoutTesting::InspectorLayoutFixture;

namespace
{

/// More channels than the list shows at its 320 px cap, so it scrolls.
constexpr int kChannelsToScroll = 30;
constexpr float kLayoutTolerancePx = 0.5f;
constexpr int kSettleFrames = 3;
constexpr float kFrameSeconds = 1.0f / 60.0f;

template <typename T>
T* FindIn(UIElement& root, const char* id)
{
    return dynamic_cast<T*>(root.FindById(GameEngine::HashStringId(id)));
}

/// An open Channels list in a window tall enough to hold it below its anchor.
struct ChannelListLayout
{
    LogView View;
    InspectorLayoutFixture Fixture;
    LogChannelPopover* Popover = nullptr;
    UIElement* Anchor = nullptr;

    bool Build()
    {
        for (int i = 0; i < kChannelsToScroll; ++i)
            Logger::RegisterChannel("Tests.ChannelLayout." + std::to_string(i), Logger::LogLevel::Info);
        auto root = std::make_unique<UIElement>();
        auto anchor = std::make_unique<UIElement>();
        Anchor = anchor.get();
        auto popover = std::make_unique<LogChannelPopover>(View);
        Popover = popover.get();
        root->AddChild(std::move(anchor));
        root->AddChild(std::move(popover));
        Fixture.ViewportH = 700;
        Fixture.ExtraSheets = {"Assets/UI/theme/views.css", "Assets/UI/theme/widgets.css",
                               "Assets/UI/theme/toggle.css", "Assets/UI/panels/LogPanel/LogChannelPopover.css"};
        if (!Fixture.Build(std::move(root)))
            return false;
        Popover->Open(*Anchor);
        Settle();
        return true;
    }

    void Settle()
    {
        for (int i = 0; i < kSettleFrames; ++i)
            Fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);
    }

    void Type(const std::string& text)
    {
        auto* field = FindIn<GameEngine::TextField>(*Popover, "log-channel-search-field");
        ASSERT_NE(field, nullptr);
        field->OnFocusChanged(true);
        for (const char character : text)
            field->OnChar(static_cast<unsigned char>(character));
        Settle();
    }

    bool Scrolls()
    {
        auto* scroll = FindIn<GameEngine::ScrollView>(*Popover, "LogChannelsScroll");
        auto* bar = scroll ? dynamic_cast<GameEngine::Scrollbar*>(scroll->GetVerticalScrollbar()) : nullptr;
        return bar && !bar->IsHidden();
    }

    /// The level list of the first row the search lists.
    Dropdown* FirstListedLevel()
    {
        auto* scroll = FindIn<GameEngine::ScrollView>(*Popover, "LogChannelsScroll");
        if (!scroll)
            return nullptr;
        for (const auto& row : scroll->GetViewport()->GetChildren())
        {
            if (!row->HasClass("hidden"))
                return FindIn<Dropdown>(*row, "LogChannelLevel");
        }
        return nullptr;
    }
};

uint32_t NameColor(UIElement& row)
{
    return FindIn<GameEngine::Label>(row, "LogChannelName")->GetResolvedStyle().Visual.Color;
}

uint32_t LevelColor(UIElement& row)
{
    return FindIn<Dropdown>(row, "LogChannelLevel")->GetHeaderLabel()->GetResolvedStyle().Visual.Color;
}

} // namespace

TEST(LogChannelPopoverLayout, TheLevelColumnLinesUpWithItsHeadingAndStaysInPlaceWhenTheListStopsScrolling)
{
    ChannelListLayout layout;
    if (!layout.Build())
    {
        if (layout.Fixture.Diagnostic == "no Vulkan device")
            GTEST_SKIP() << layout.Fixture.Diagnostic;
        FAIL() << layout.Fixture.Diagnostic;
    }
    UIElement* heading = FindByClass(layout.Popover, "log-channels-column-level");
    ASSERT_NE(heading, nullptr);
    Dropdown* level = layout.FirstListedLevel();
    ASSERT_NE(level, nullptr);
    ASSERT_GT(level->GetLayoutWidth(), 0.0f) << "the list did not lay out; this test is vacuous";
    ASSERT_TRUE(layout.Scrolls()) << "the list must scroll for this test to mean anything";
    const float scrolledX = level->GetWindowRect().X;
    EXPECT_NEAR(heading->GetWindowRect().X, scrolledX, kLayoutTolerancePx);
    const UIElement* row = level->GetParent();
    const UIElement* viewport = row->GetParent()->GetParent();
    EXPECT_LE(row->GetWindowRect().Right(), viewport->GetWindowRect().Right() + kLayoutTolerancePx)
        << "the row is wider than the room beside the scrollbar";

    layout.Type("channellayout.7");
    ASSERT_FALSE(layout.Scrolls()) << "one listed row must not scroll";
    level = layout.FirstListedLevel();
    ASSERT_NE(level, nullptr);
    EXPECT_NEAR(level->GetWindowRect().X, scrolledX, kLayoutTolerancePx) << "the Level column moved";
    EXPECT_NEAR(heading->GetWindowRect().X, scrolledX, kLayoutTolerancePx);
}

TEST(LogChannelPopoverLayout, AHiddenChannelDimsItsNameAndItsLevelAlike)
{
    ChannelListLayout layout;
    if (!layout.Build())
    {
        if (layout.Fixture.Diagnostic == "no Vulkan device")
            GTEST_SKIP() << layout.Fixture.Diagnostic;
        FAIL() << layout.Fixture.Diagnostic;
    }
    layout.Type("channellayout.");
    auto* scroll = FindIn<GameEngine::ScrollView>(*layout.Popover, "LogChannelsScroll");
    ASSERT_NE(scroll, nullptr);
    std::vector<UIElement*> listed;
    for (const auto& row : scroll->GetViewport()->GetChildren())
    {
        if (!row->HasClass("hidden"))
            listed.push_back(row.get());
    }
    ASSERT_GE(listed.size(), 2u);
    UIElement* shownRow = listed[0];
    UIElement* hiddenRow = listed[1];
    auto* toggle = FindIn<GameEngine::Toggle>(*hiddenRow, "LogChannelShown");
    ASSERT_NE(toggle, nullptr);
    toggle->SetValue(false);
    layout.Settle();

    EXPECT_NE(NameColor(*hiddenRow), NameColor(*shownRow)) << "the hidden channel's name does not dim";
    EXPECT_EQ(LevelColor(*hiddenRow), NameColor(*hiddenRow)) << "the level is not dimmed as the name is";
    EXPECT_NE(LevelColor(*shownRow), LevelColor(*hiddenRow));
}

// The editor runs at the Debug level, so Core's level list leads with "Default (Debug)", its longest item.
TEST(LogChannelPopoverLayout, TheLongestLevelItemKeepsOneLine)
{
    const Logger::LogLevel globalLevel = Logger::Log::GetLogLevel();
    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    ChannelListLayout layout;
    const bool built = layout.Build();
    Logger::Log::SetLogLevel(globalLevel);
    if (!built)
    {
        if (layout.Fixture.Diagnostic == "no Vulkan device")
            GTEST_SKIP() << layout.Fixture.Diagnostic;
        FAIL() << layout.Fixture.Diagnostic;
    }
    Dropdown* coreLevel = layout.FirstListedLevel();
    ASSERT_NE(coreLevel, nullptr);
    ASSERT_EQ(coreLevel->GetSelectedLabel(), "Default (Debug)");
    coreLevel->OpenMenuUi();
    layout.Settle();
    UIElement* items = coreLevel->GetItemsContainer();
    ASSERT_NE(items, nullptr);
    ASSERT_GE(items->GetChildren().size(), 2u);
    const float defaultHeight = items->GetChildren()[0]->GetLayoutHeight();
    ASSERT_GT(defaultHeight, 0.0f) << "the level list did not lay out";
    EXPECT_NEAR(defaultHeight, items->GetChildren()[1]->GetLayoutHeight(), kLayoutTolerancePx)
        << "the Default item wraps to a second line";
}
