// The Log panel's Channels list: Core first and the other channels by name, a search that lists the channels whose
// name contains it with a count, a row's toggle that hides the channel's lines, Show All and Hide All on the listed
// rows, and a row's level that reaches the logger. The layouts are staged beside the test.

#include <gtest/gtest.h>

#include "Input/KeyCodes.h"
#include "Logger/LogChannel.h"
#include "Logger/Logger.h"
#include "Panels/LogChannelPopover.h"
#include "Panels/LogView.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using GameEngine::Dropdown;
using GameEngine::Label;
using GameEngine::LogChannelPopover;
using GameEngine::LogView;
using GameEngine::Toggle;
using GameEngine::UIElement;
using Logger::LogLevel;

namespace
{

/// The level list's items: Default first, then Trace, Debug, Info, Warning, Error and Off.
constexpr int kDefaultItem = 0;
constexpr int kWarningItem = 4;

template <typename T>
T* FindIn(UIElement& root, const char* id)
{
    return dynamic_cast<T*>(root.FindById(GameEngine::HashStringId(id)));
}

/// An open list with two test channels registered, Zeta before Alpha.
class LogChannelPopoverTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Zeta = Logger::RegisterChannel("Tests.ChannelList.Zeta", LogLevel::Info).Index;
        m_Alpha = Logger::RegisterChannel("Tests.ChannelList.Alpha", LogLevel::Info).Index;
        m_Popover.Open(m_Anchor);
        auto* scroll = FindIn<GameEngine::ScrollView>(m_Popover, "LogChannelsScroll");
        ASSERT_NE(scroll, nullptr) << "the list did not build from its layout";
        m_Rows = scroll->GetViewport();
        ASSERT_NE(m_Rows, nullptr) << "the list did not build from its layout";
    }

    void TearDown() override
    {
        Logger::Log::SetChannelLevel(m_Alpha, std::nullopt);
        Logger::Log::SetChannelLevel(m_Zeta, std::nullopt);
    }

    static std::string NameOf(UIElement& row)
    {
        Label* name = FindIn<Label>(row, "LogChannelName");
        return name ? name->GetText() : std::string();
    }

    std::vector<std::string> ListedNames() const
    {
        std::vector<std::string> names;
        for (const auto& row : m_Rows->GetChildren())
        {
            if (!row->HasClass("hidden"))
                names.push_back(NameOf(*row));
        }
        return names;
    }

    UIElement* RowNamed(const std::string& name) const
    {
        for (const auto& row : m_Rows->GetChildren())
        {
            if (NameOf(*row) == name)
                return row.get();
        }
        return nullptr;
    }

    GameEngine::TextField& SearchField()
    {
        auto* field = FindIn<GameEngine::TextField>(m_Popover, "log-channel-search-field");
        EXPECT_NE(field, nullptr) << "the list has no search field";
        return *field;
    }

    /// Types `text` into the search field as the keyboard does, after the field takes the focus.
    void Type(const std::string& text)
    {
        GameEngine::TextField& field = SearchField();
        field.OnFocusChanged(true);
        for (const char character : text)
            field.OnChar(static_cast<unsigned char>(character));
    }

    /// True when the search field takes the key; false when it leaves it to the list, which then closes.
    bool PressEscape() { return SearchField().OnKey(GameEngine::Input::kKeyCode_Escape, 0, nullptr); }

    void Click(const char* id)
    {
        auto* button = FindIn<GameEngine::Button>(m_Popover, id);
        ASSERT_NE(button, nullptr) << id;
        GameEngine::UIEvent click{};
        click.Id = GameEngine::kEventButtonClick;
        click.Target = button;
        click.CurrentTarget = button;
        button->DispatchEvent(click);
    }

    LogView m_View;
    LogChannelPopover m_Popover{m_View};
    UIElement m_Anchor;
    UIElement* m_Rows = nullptr;
    uint16_t m_Zeta = 0;
    uint16_t m_Alpha = 0;
};

} // namespace

TEST_F(LogChannelPopoverTest, CoreIsFirstAndTheOtherChannelsFollowByNameIgnoringCase)
{
    Logger::RegisterChannel("tests.channellist.beta", LogLevel::Info);
    m_Popover.Close();
    m_Popover.Open(m_Anchor);
    const std::vector<std::string> names = ListedNames();
    ASSERT_EQ(names.size(), Logger::Log::GetChannelCount());
    EXPECT_EQ(names.front(), "Core");
    const auto alpha = std::ranges::find(names, "Tests.ChannelList.Alpha");
    const auto zeta = std::ranges::find(names, "Tests.ChannelList.Zeta");
    ASSERT_NE(alpha, names.end());
    ASSERT_NE(zeta, names.end());
    const auto beta = std::ranges::find(names, "tests.channellist.beta");
    ASSERT_NE(beta, names.end());
    EXPECT_LT(alpha, beta);
    EXPECT_LT(beta, zeta);
}

TEST_F(LogChannelPopoverTest, TheSearchListsTheChannelsWhoseNameContainsItAndCountsThem)
{
    auto* count = FindIn<Label>(m_Popover, "LogChannelsCount");
    ASSERT_NE(count, nullptr);
    EXPECT_TRUE(count->HasClass("hidden")) << "no count line without a search";
    auto* search = dynamic_cast<GameEngine::SearchFieldWithFilter*>(SearchField().GetParent());
    ASSERT_NE(search, nullptr);
    EXPECT_TRUE(search->HasClass("context-menu-search")) << "the field of the searchable menus";
    EXPECT_TRUE(search->HasClass("awaiting-input")) << "no focus ring before the user types";

    Type("channellist.AL");
    EXPECT_FALSE(search->HasClass("awaiting-input"));
    EXPECT_EQ(ListedNames(), (std::vector<std::string>{"Tests.ChannelList.Alpha"}));
    EXPECT_FALSE(count->HasClass("hidden"));
    EXPECT_TRUE(FindIn<UIElement>(m_Popover, "LogChannelsShowAll")->IsEnabled());
    EXPECT_EQ(count->GetText(), "1 of " + std::to_string(Logger::Log::GetChannelCount()) + " channels");

    Type("x");
    EXPECT_TRUE(ListedNames().empty());
    EXPECT_EQ(count->GetText(), "No channel matches");
    EXPECT_TRUE(FindIn<UIElement>(m_Popover, "LogChannelsColumns")->HasClass("hidden")) << "headings over no rows";
    EXPECT_FALSE(FindIn<UIElement>(m_Popover, "LogChannelsShowAll")->IsEnabled());
    EXPECT_FALSE(FindIn<UIElement>(m_Popover, "LogChannelsHideAll")->IsEnabled());
}

TEST_F(LogChannelPopoverTest, EscapeClearsTheSearchAndTheNextEscapeIsLeftToTheList)
{
    Type("zeta");
    ASSERT_EQ(ListedNames(), (std::vector<std::string>{"Tests.ChannelList.Zeta"}));

    EXPECT_TRUE(PressEscape());
    EXPECT_EQ(SearchField().GetValue(), "");
    EXPECT_EQ(ListedNames().size(), Logger::Log::GetChannelCount());
    EXPECT_TRUE(FindIn<Label>(m_Popover, "LogChannelsCount")->HasClass("hidden"));

    EXPECT_FALSE(PressEscape()) << "with nothing to clear, Escape goes on to close the list";
}

TEST_F(LogChannelPopoverTest, ARowsToggleHidesTheChannelsLinesAndHideAllActsOnTheListedRowsOnly)
{
    UIElement* alphaRow = RowNamed("Tests.ChannelList.Alpha");
    ASSERT_NE(alphaRow, nullptr);
    auto* shown = FindIn<Toggle>(*alphaRow, "LogChannelShown");
    ASSERT_NE(shown, nullptr);
    EXPECT_TRUE(shown->GetValue());

    shown->SetValue(false);
    EXPECT_FALSE(m_View.GetModel().IsChannelShown(m_Alpha));
    EXPECT_TRUE(alphaRow->HasClass("log-channel-row-off"));

    Click("LogChannelsShowAll");
    EXPECT_TRUE(m_View.GetModel().IsChannelShown(m_Alpha));
    EXPECT_TRUE(shown->GetValue()) << "the row shows the state Show All set";

    Type("channellist.zeta");
    Click("LogChannelsHideAll");
    EXPECT_FALSE(m_View.GetModel().IsChannelShown(m_Zeta));
    EXPECT_TRUE(m_View.GetModel().IsChannelShown(m_Alpha)) << "a row the search does not list keeps its state";
    EXPECT_EQ(m_View.GetModel().CountHiddenChannels(), 1u);
}

TEST_F(LogChannelPopoverTest, ALevelChosenOnARowReachesTheLogger)
{
    UIElement* alphaRow = RowNamed("Tests.ChannelList.Alpha");
    ASSERT_NE(alphaRow, nullptr);
    auto* level = FindIn<Dropdown>(*alphaRow, "LogChannelLevel");
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->GetSelectedIndex(), kDefaultItem);
    EXPECT_EQ(level->GetSelectedLabel().rfind("Default (", 0), 0u) << level->GetSelectedLabel();
    EXPECT_NE(level->GetTooltip().find("Default uses the editor's log level"), std::string::npos)
        << "the tooltip says where the level in parentheses comes from";

    level->SetSelectedIndex(kWarningItem);
    EXPECT_EQ(Logger::Log::GetChannelSetting(m_Alpha), std::optional<LogLevel>(LogLevel::Warning));

    level->SetSelectedIndex(kDefaultItem);
    EXPECT_EQ(Logger::Log::GetChannelSetting(m_Alpha), std::nullopt);
}

TEST_F(LogChannelPopoverTest, AReopenedListShowsTheHiddenChannelsAndAChannelRegisteredAfterHideAllIsShown)
{
    Click("LogChannelsHideAll");
    const uint16_t late = Logger::RegisterChannel("Tests.ChannelList.Late", LogLevel::Info).Index;
    m_Popover.Close();
    m_Popover.Open(m_Anchor);

    UIElement* alphaRow = RowNamed("Tests.ChannelList.Alpha");
    UIElement* lateRow = RowNamed("Tests.ChannelList.Late");
    ASSERT_NE(alphaRow, nullptr);
    ASSERT_NE(lateRow, nullptr);
    EXPECT_FALSE(FindIn<Toggle>(*alphaRow, "LogChannelShown")->GetValue());
    EXPECT_TRUE(m_View.GetModel().IsChannelShown(late)) << "Hide All acts on the channels it listed";
    EXPECT_TRUE(FindIn<Toggle>(*lateRow, "LogChannelShown")->GetValue());
}
