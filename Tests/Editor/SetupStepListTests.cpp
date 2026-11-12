// The setup step list: the summary head, one row per step from its row layout, the step's mark,
// name and finding, the fix in an error or warning notice only for a failed or warning step, and no
// row of an earlier check after SetSteps.

#include <gtest/gtest.h>

#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SetupStepList.h"
#include "UI/UIElement.h"

#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::EditorUI::SetupStepList;

namespace
{

const UIElement* FindWithClass(const UIElement& root, const char* className)
{
    if (root.HasClass(className))
        return &root;
    for (const auto& child : root.GetChildren())
        if (const UIElement* found = FindWithClass(*child, className))
            return found;
    return nullptr;
}

std::string TextOf(const UIElement& row, const char* className)
{
    const auto* label = dynamic_cast<const Label*>(FindWithClass(row, className));
    return label ? label->GetText() : std::string("<no " + std::string(className) + ">");
}

// The step rows, under the rows container that follows the summary head.
const std::vector<std::unique_ptr<UIElement>>& RowsOf(const SetupStepList& list)
{
    static const std::vector<std::unique_ptr<UIElement>> kNone;
    const UIElement* rows = FindWithClass(list, "setup-step-rows");
    return rows ? rows->GetChildren() : kNone;
}

std::vector<SetupStepList::Step> FailedProgram()
{
    return {{"Claude Code", SetupStepList::State::Failed, "This program is not Claude Code: it printed nothing.",
             "Click Browse to pick Claude Code's claude.exe."},
            {"Logged in", SetupStepList::State::Waiting, "Checked once Claude Code is found.", {}},
            {"Editor tools", SetupStepList::State::Warning, "Node.js did not run.", "Install Node 20 or later."}};
}

} // namespace

TEST(SetupStepList, ShowsOneRowPerStepWithItsMarkNameAndFinding)
{
    SetupStepList list;
    list.SetSteps(FailedProgram());
    const auto& rows = RowsOf(list);
    ASSERT_EQ(rows.size(), 3u) << "SetupStepList.uxml and SetupStepRow.uxml are staged beside the test "
                                  "executable under Assets/UI/controls/SetupStepList/";
    const char* marks[] = {"setup-step-mark-failed", "setup-step-mark-waiting", "setup-step-mark-warning"};
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const UIElement* mark = FindWithClass(*rows[i], "setup-step-mark");
        ASSERT_NE(mark, nullptr);
        EXPECT_TRUE(mark->HasClass(marks[i])) << "row " << i;
        EXPECT_EQ(TextOf(*rows[i], "setup-step-name"), FailedProgram()[i].Name);
        EXPECT_EQ(TextOf(*rows[i], "setup-step-found"), FailedProgram()[i].Found);
        EXPECT_EQ(rows[i]->HasClass("setup-step-row-divided"), i > 0) << "a line between rows only";
    }
    EXPECT_TRUE(FindWithClass(*rows[1], "setup-step-found-waiting")) << "a waiting step's line is dim";
}

TEST(SetupStepList, AFailedOrWarningStepShowsItsFixInANoticeOfItsKind)
{
    SetupStepList list;
    list.SetSteps(FailedProgram());
    const auto& rows = RowsOf(list);
    ASSERT_EQ(rows.size(), 3u);
    const UIElement* error = FindWithClass(*rows[0], "inspector-notice-error");
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(TextOf(*error, "inspector-notice-text"), FailedProgram()[0].Fix);
    EXPECT_EQ(FindWithClass(*rows[1], "inspector-notice"), nullptr) << "a waiting step has no fix";
    const UIElement* warning = FindWithClass(*rows[2], "inspector-notice-warning");
    ASSERT_NE(warning, nullptr);
    EXPECT_EQ(TextOf(*warning, "inspector-notice-text"), FailedProgram()[2].Fix);
}

TEST(SetupStepList, SetStepsReplacesTheRowsOfTheEarlierCheck)
{
    SetupStepList list;
    list.SetSteps(FailedProgram());
    const std::vector<SetupStepList::Step> ready = {
        {"Claude Code", SetupStepList::State::Passed, "Claude Code 2.1.300 at C:/Tools/claude.exe.", {}},
        {"Logged in", SetupStepList::State::Passed, "Logged in (claude.ai).", "never shown"}};
    list.SetSteps(ready);
    const auto& rows = RowsOf(list);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(FindWithClass(*rows[0], "setup-step-mark-passed"));
    EXPECT_EQ(TextOf(*rows[1], "setup-step-found"), "Logged in (claude.ai).");
    EXPECT_EQ(FindWithClass(list, "inspector-notice"), nullptr) << "a passed step shows no fix";
}

// The head shows one pill state class at a time (none before a check) and the line beside it;
// no steps hide the rows' frame, and Check again calls back.
TEST(SetupStepList, TheHeadShowsTheSummaryAndCheckAgainCallsBack)
{
    SetupStepList list;
    const UIElement* pill = FindWithClass(list, "setup-status-pill");
    const UIElement* rows = FindWithClass(list, "setup-step-rows");
    ASSERT_NE(pill, nullptr) << "SetupStepList.uxml is staged beside the test executable";
    ASSERT_NE(rows, nullptr);
    EXPECT_TRUE(rows->HasClass("hidden")) << "no steps, no frame";

    list.SetSummary(SetupStepList::Summary::ReadyLimited, "Ready, limited", "Conversation only.");
    EXPECT_TRUE(pill->HasClass("setup-status-pill-warning"));
    EXPECT_EQ(TextOf(list, "setup-status-pill"), "Ready, limited");
    EXPECT_EQ(TextOf(list, "setup-step-summary"), "Conversation only.");

    list.SetSummary(SetupStepList::Summary::NotChecked, "Not checked", {});
    for (const char* state : {"checking", "ready", "warning", "failed"})
        EXPECT_FALSE(pill->HasClass(std::string("setup-status-pill-") + state)) << state;

    list.SetSteps(FailedProgram());
    EXPECT_FALSE(rows->HasClass("hidden"));

    int clicks = 0;
    list.SetOnCheckAgain([&clicks] { ++clicks; });
    Button* checkAgain = list.FindById<Button>(HashStringId("SetupStepCheckAgain")).Get();
    ASSERT_NE(checkAgain, nullptr);
    checkAgain->TriggerClick();
    EXPECT_EQ(clicks, 1);
}
