// The Log panel's list keeps its selection only on lines it shows: a filter that hides a selected
// line, Clear, and old lines dropped past the maximum take them out of the selection, and the
// selection callback reports the change so the panel closes its detail pane.

#include <gtest/gtest.h>

#include "Logger/LogSink.h"
#include "Panels/LogModel.h"
#include "Panels/LogView.h"
#include "UI/Controls/ListView.h"
#include "UI/UIElement.h"

#include <string>
#include <vector>

using GameEngine::ListView;
using GameEngine::LogModel;
using GameEngine::LogView;
using Logger::LogLevel;

namespace
{

constexpr const char* kListId = "LogListView";

/// A list with one error line at row 0 and one info line at row 1.
class LogViewSelection : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_View.SetOnSelectionChanged(
            [this](const LogModel::Entry* entry)
            {
                ++m_SelectionChanges;
                m_LastSelected = entry;
            });
        Add(LogLevel::Error, "error line");
        Add(LogLevel::Info, "info line");
        m_View.FlushPendingMessages();
        m_List = dynamic_cast<ListView*>(m_View.FindById(GameEngine::HashStringId(kListId)));
        ASSERT_NE(m_List, nullptr) << "the list did not build its ListView";
    }

    void Add(LogLevel level, const std::string& text) { m_View.AddMessage(Logger::LogMessage(level, text)); }

    std::vector<std::string> SelectedTexts() const
    {
        std::vector<std::string> texts;
        for (const LogModel::Entry* entry : m_View.GetSelectedEntries())
            texts.push_back(entry->Text);
        return texts;
    }

    LogView m_View;
    ListView* m_List = nullptr;
    int m_SelectionChanges = 0;
    const LogModel::Entry* m_LastSelected = nullptr;
};

} // namespace

TEST_F(LogViewSelection, AFilterThatHidesTheSelectedLineClearsTheSelection)
{
    m_List->SetSelectedIndex(0, /*scrollIntoView=*/false);
    ASSERT_NE(m_View.GetSelectedEntry(), nullptr);
    ASSERT_EQ(m_View.GetSelectedEntry()->Text, "error line");

    m_View.SetSeverityShown(LogModel::Severity::Error, false);

    EXPECT_EQ(m_View.GetSelectedEntry(), nullptr);
    EXPECT_TRUE(m_View.GetSelectedEntries().empty());
    EXPECT_EQ(m_LastSelected, nullptr) << "the panel was not told, so its detail pane stays open";
}

TEST_F(LogViewSelection, AFilterThatHidesOneOfTheSelectedLinesKeepsOnlyTheShownOnes)
{
    m_List->SetSelectedIndex(1, /*scrollIntoView=*/false);
    m_List->SetSelectedIndexKeyboard(0, /*extendRange=*/true, /*scrollIntoView=*/false);
    ASSERT_EQ(SelectedTexts(), (std::vector<std::string>{"error line", "info line"}));

    m_View.SetSeverityShown(LogModel::Severity::Error, false);

    EXPECT_EQ(SelectedTexts(), std::vector<std::string>{"info line"}) << "a copy would take a line the list hides";
    ASSERT_NE(m_View.GetSelectedEntry(), nullptr);
    EXPECT_EQ(m_View.GetSelectedEntry()->Text, "info line");
}

TEST_F(LogViewSelection, ClearDropsTheSelection)
{
    m_List->SetSelectedIndex(1, /*scrollIntoView=*/false);
    ASSERT_NE(m_View.GetSelectedEntry(), nullptr);

    m_View.Clear();

    EXPECT_EQ(m_View.GetSelectedEntry(), nullptr);
    EXPECT_TRUE(m_View.GetSelectedEntries().empty());
    EXPECT_EQ(m_LastSelected, nullptr);
}

TEST_F(LogViewSelection, ASelectedLineDroppedPastTheMaximumLeavesTheSelection)
{
    m_List->SetSelectedIndex(0, /*scrollIntoView=*/false);
    ASSERT_NE(m_View.GetSelectedEntry(), nullptr);
    const int changesBefore = m_SelectionChanges;

    for (size_t index = 0; index < LogModel::kDefaultMaxEntries; ++index)
        Add(LogLevel::Info, "filler");
    m_View.FlushPendingMessages();

    EXPECT_EQ(m_View.GetSelectedEntry(), nullptr);
    EXPECT_TRUE(m_View.GetSelectedEntries().empty());
    EXPECT_GT(m_SelectionChanges, changesBefore);
    EXPECT_EQ(m_LastSelected, nullptr);
}
