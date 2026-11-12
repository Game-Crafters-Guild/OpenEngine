// The Build panel's Requirements rows over stand-in commands: a row follows its check, Install runs the
// install command as a background job and checks again when it ends or is cancelled, and a failed
// install says why and offers Install again. Jobs wait in a queue until the test runs them.

#include <gtest/gtest.h>

#include "Engine/Build/ExportRequirements.h"
#include "InspectorLayoutFixture.h"
#include "Panels/BuildPanel/ExportRequirementsSection.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
using InspectorLayoutTesting::FindByClass;

namespace
{

constexpr std::array<std::string_view, 1> kChecks{"check-tool"};
constexpr ExportRequirement kTool{
    .Name = "Test Tool",
    .Platform = "Mac",
    .CheckCommands = kChecks,
    .InstallCommand = "install-tool",
    .Fallback = "Without it, the export skips a step.",
};

// Stand-in commands and a job queue the test drains by hand.
struct FakeWork
{
    int CheckExitCode = 1;
    int InstallExitCode = 0;
    std::vector<std::string> InstallOutput;
    // Runs after the install command's output, before it exits.
    std::function<void()> WhileInstalling;
    // The running install's stop test. Valid only while the install runs.
    std::function<bool()> InstallShouldStop;
    std::vector<std::string> Ran;
    std::vector<std::function<void()>> Queued;

    ExportRequirementWork Make()
    {
        ExportRequirementWork work;
        work.RunCommand = [this](const std::string& command, const std::function<void(const std::string&)>& onLine,
                                 const std::function<bool()>& shouldStop) {
            Ran.push_back(command);
            if (command != kTool.InstallCommand)
                return CheckExitCode;
            InstallShouldStop = shouldStop;
            for (const std::string& line : InstallOutput)
                onLine(line);
            if (WhileInstalling)
                WhileInstalling();
            return InstallExitCode;
        };
        work.Start = [this](std::function<void()> job) { Queued.push_back(std::move(job)); };
        work.Post = [](std::function<void()> action) { action(); };
        return work;
    }

    void RunQueued()
    {
        std::vector<std::function<void()>> jobs = std::move(Queued);
        Queued.clear();
        for (std::function<void()>& job : jobs)
            job();
    }
};

std::string TitleOf(UIElement& section)
{
    auto* title = dynamic_cast<Label*>(FindByClass(&section, "inspector-notice-title"));
    return title ? title->GetText() : std::string();
}

std::string TextOf(UIElement& section)
{
    auto* text = dynamic_cast<Label*>(FindByClass(&section, "inspector-notice-text"));
    return text ? text->GetText() : std::string();
}

Button* ActionOf(UIElement& section)
{
    auto* action = dynamic_cast<Button*>(FindByClass(&section, "inspector-notice-action"));
    return action && !action->HasClass("hidden") ? action : nullptr;
}

// The section in a UI tree, as in the editor. Declare the FakeWork first: the section uses its commands.
struct MountedSection
{
    explicit MountedSection(ExportRequirementWork work)
    {
        auto section = std::make_unique<ExportRequirementsSection>(std::move(work));
        Section = section.get();
        auto root = std::make_unique<UIElement>();
        root->AddChild(std::move(section));
        Manager.SetRoot(std::move(root));
    }

    // Clicks as UIManager does: the handlers run inside the dispatch guard, and the actions they post run after.
    void Click(Button& button)
    {
        UIElement::SetInEventDispatch(true);
        button.TriggerClick();
        UIElement::SetInEventDispatch(false);
        Manager.DrainDeferredActionsOnce();
    }

    UIManager Manager{nullptr};
    ExportRequirementsSection* Section = nullptr;
};

TEST(ExportRequirementsSectionTests, ARowShowsWhatItsCheckFinds)
{
    FakeWork work;
    ExportRequirementsSection section(work.Make());
    section.ShowRequirements({&kTool});
    EXPECT_EQ(TitleOf(section), "Test Tool: checking");
    EXPECT_EQ(TextOf(section), "");
    ASSERT_EQ(work.Queued.size(), 1u);

    work.RunQueued();
    EXPECT_EQ(work.Ran, (std::vector<std::string>{"check-tool"}));
    EXPECT_EQ(TitleOf(section), "Test Tool: not installed (optional)");
    EXPECT_EQ(TextOf(section), kTool.Fallback);
    ASSERT_NE(ActionOf(section), nullptr);
    EXPECT_EQ(ActionOf(section)->GetText(), "Install");

    work.CheckExitCode = 0;
    section.Recheck();
    work.RunQueued();
    EXPECT_EQ(TitleOf(section), "Test Tool: installed");
    EXPECT_EQ(TextOf(section), "");
    EXPECT_EQ(ActionOf(section), nullptr);
}

TEST(ExportRequirementsSectionTests, InstallRunsInTheBackgroundAndChecksAgainWhenItEnds)
{
    FakeWork work;
    MountedSection mounted(work.Make());
    ExportRequirementsSection& section = *mounted.Section;
    section.ShowRequirements({&kTool});
    work.RunQueued();
    ASSERT_NE(ActionOf(section), nullptr);

    mounted.Click(*ActionOf(section));
    EXPECT_EQ(TitleOf(section), "Test Tool: installing");
    EXPECT_EQ(work.Ran, (std::vector<std::string>{"check-tool"}));
    ASSERT_EQ(work.Queued.size(), 1u);
    ASSERT_NE(ActionOf(section), nullptr);
    EXPECT_EQ(ActionOf(section)->GetText(), "Cancel");

    work.InstallOutput = {"Downloading... (40%)"};
    std::string progress;
    work.WhileInstalling = [&]() { progress = TextOf(section); };
    work.CheckExitCode = 0;
    work.RunQueued();
    EXPECT_EQ(progress, "Downloading... (40%)");
    EXPECT_EQ(work.Ran, (std::vector<std::string>{"check-tool", "install-tool"}));
    EXPECT_EQ(TitleOf(section), "Test Tool: checking");
    ASSERT_EQ(work.Queued.size(), 1u);

    work.RunQueued();
    EXPECT_EQ(work.Ran, (std::vector<std::string>{"check-tool", "install-tool", "check-tool"}));
    EXPECT_EQ(TitleOf(section), "Test Tool: installed");
}

TEST(ExportRequirementsSectionTests, ACancelledInstallChecksAgainInsteadOfFailing)
{
    FakeWork work;
    MountedSection mounted(work.Make());
    ExportRequirementsSection& section = *mounted.Section;
    section.ShowRequirements({&kTool});
    work.RunQueued();
    mounted.Click(*ActionOf(section));
    ASSERT_NE(ActionOf(section), nullptr);
    mounted.Click(*ActionOf(section));
    // A stopped command exits with a code that is not 0.
    work.InstallExitCode = 137;
    work.RunQueued();

    EXPECT_EQ(TitleOf(section), "Test Tool: checking");
    EXPECT_EQ(work.Queued.size(), 1u);
}

TEST(ExportRequirementsSectionTests, InstallOutputKeepsTheCancelButton)
{
    FakeWork work;
    MountedSection mounted(work.Make());
    ExportRequirementsSection& section = *mounted.Section;
    section.ShowRequirements({&kTool});
    work.RunQueued();
    mounted.Click(*ActionOf(section));
    ASSERT_NE(ActionOf(section), nullptr);
    // A press and its release must land on one button while output lines arrive.
    ActionOf(section)->AddClass("test-pressed");
    work.InstallOutput = {"Downloading... (10%)", "Downloading... (20%)"};
    work.WhileInstalling = [&]() {
        ASSERT_NE(ActionOf(section), nullptr);
        EXPECT_TRUE(ActionOf(section)->HasClass("test-pressed"));
        EXPECT_EQ(TextOf(section), "Downloading... (20%)");
        mounted.Click(*ActionOf(section));
    };
    work.InstallExitCode = 137;
    work.RunQueued();

    EXPECT_EQ(TitleOf(section), "Test Tool: checking");
}

TEST(ExportRequirementsSectionTests, AFailedInstallSaysWhyAndOffersInstallAgain)
{
    FakeWork work;
    MountedSection mounted(work.Make());
    ExportRequirementsSection& section = *mounted.Section;
    section.ShowRequirements({&kTool});
    work.RunQueued();
    mounted.Click(*ActionOf(section));
    work.InstallExitCode = 1;
    work.InstallOutput = {"", "xcode-select: error: tool 'xcodebuild' requires Xcode"};
    work.RunQueued();

    EXPECT_EQ(TitleOf(section), "Test Tool: install failed");
    EXPECT_NE(TextOf(section).find("'install-tool' exited with code 1: xcode-select: error: tool 'xcodebuild' requires Xcode"),
              std::string::npos)
        << TextOf(section);
    ASSERT_NE(ActionOf(section), nullptr);
    EXPECT_EQ(ActionOf(section)->GetText(), "Install");
    EXPECT_TRUE(work.Queued.empty());

    mounted.Click(*ActionOf(section));
    EXPECT_EQ(TitleOf(section), "Test Tool: installing");
    EXPECT_EQ(work.Queued.size(), 1u);
}

// The rows change after the click ends, so the clicked button is the one row's button until its handler returns.
TEST(ExportRequirementsSectionTests, InstallKeepsTheClickedButtonUntilTheClickEnds)
{
    FakeWork work;
    ExportRequirementWork commands = work.Make();
    UIElement* rows = nullptr;
    std::size_t rowsInClick = 0;
    uint64_t buttonInClick = 0;
    // Install starts its job inside the click, after it asks for the new rows.
    commands.Start = [&, start = commands.Start](std::function<void()> job) {
        if (rows)
        {
            rowsInClick = rows->GetChildren().size();
            buttonInClick = ActionOf(*rows) ? ActionOf(*rows)->GetInstanceId() : 0;
        }
        start(std::move(job));
    };
    MountedSection mounted(std::move(commands));
    ExportRequirementsSection& section = *mounted.Section;
    section.ShowRequirements({&kTool});
    work.RunQueued();
    rows = FindByClass(&section, "export-requirements-rows");
    ASSERT_NE(rows, nullptr);
    ASSERT_NE(ActionOf(section), nullptr);
    const uint64_t install = ActionOf(section)->GetInstanceId();

    mounted.Click(*ActionOf(section));
    EXPECT_EQ(rowsInClick, 1u);
    EXPECT_EQ(buttonInClick, install);
    EXPECT_EQ(rows->GetChildren().size(), 1u);
    EXPECT_EQ(TitleOf(section), "Test Tool: installing");
    ASSERT_NE(ActionOf(section), nullptr);
    EXPECT_EQ(ActionOf(section)->GetText(), "Cancel");
}

TEST(ExportRequirementsSectionTests, DocsOpensTheExportRequirementsPageInTheHost)
{
    FakeWork work;
    MountedSection mounted(work.Make());
    ExportRequirementsSection& section = *mounted.Section;
    std::string opened;
    section.SetOnOpenDocsPage([&](std::string_view page) { opened = page; });
    auto* docs = dynamic_cast<Button*>(FindByClass(&section, "export-requirements-docs"));
    ASSERT_NE(docs, nullptr);
    mounted.Click(*docs);

    EXPECT_EQ(opened, "publishing/game-publishing/#export-requirements");
}

TEST(ExportRequirementsSectionTests, ARowStillCheckingIsCheckedAgainWhenTheSectionGetsAManager)
{
    FakeWork work;
    auto owned = std::make_unique<ExportRequirementsSection>(work.Make());
    ExportRequirementsSection& section = *owned;
    section.ShowRequirements({&kTool});
    // The first check ran before the section had a manager, so its result went nowhere.
    work.Queued.clear();

    UIManager manager(nullptr);
    auto root = std::make_unique<UIElement>();
    root->AddChild(std::move(owned));
    manager.SetRoot(std::move(root));
    ASSERT_EQ(work.Queued.size(), 1u);
    work.RunQueued();

    EXPECT_EQ(TitleOf(section), "Test Tool: not installed (optional)");
}

TEST(ExportRequirementsSectionTests, ARowThatGoesStopsItsInstall)
{
    FakeWork work;
    MountedSection mounted(work.Make());
    ExportRequirementsSection& section = *mounted.Section;
    section.ShowRequirements({&kTool});
    work.RunQueued();
    mounted.Click(*ActionOf(section));
    bool kept = false;
    bool stopped = false;
    // The rows change twice: the row stays (another platform toggled), then it goes.
    work.WhileInstalling = [&]() {
        section.ShowRequirements({&kTool});
        kept = !work.InstallShouldStop();
        section.ShowRequirements({});
        stopped = work.InstallShouldStop();
    };
    work.RunQueued();

    EXPECT_TRUE(kept);
    EXPECT_TRUE(stopped);
    EXPECT_EQ(TitleOf(section), "");
    EXPECT_TRUE(work.Queued.empty());
}

} // namespace
