// get_build_status is the record of a game build, so it stays short however much the
// build reported: the errors are counted and the first unique ones listed, the warnings
// counted and listed only on request.

#include "DebugServer/BuildStatusReport.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

TEST(BuildStatusReport, CountsUniqueLinesAndListsTheFirstErrorsAndWarningsOnlyOnRequest)
{
    GameEngine::BuildPanel::StatusSnapshot status;
    status.generation = 3;
    status.outputDirectory = "C:/Out";
    auto& progress = status.progress;
    for (int i = 0; i < 25; ++i)
        progress.errors.push_back("error " + std::to_string(i));
    progress.errors.push_back("error 0");
    for (int i = 0; i < 40; ++i)
        progress.warnings.push_back("warning " + std::to_string(i));

    auto summary =
        GameEngine::Editor::DescribeBuildStatus(status, /*includeWarnings*/ false);
    EXPECT_EQ(summary["errors"], 25) << "a repeated line counts once";
    ASSERT_EQ(summary["errorLines"].size(), GameEngine::Editor::kBuildStatusMaxErrorLines);
    EXPECT_EQ(summary["errorLines"][0], "error 0");
    EXPECT_EQ(summary["warnings"], 40);
    EXPECT_FALSE(summary.contains("warningLines"));

    auto withWarnings =
        GameEngine::Editor::DescribeBuildStatus(status, /*includeWarnings*/ true);
    EXPECT_EQ(withWarnings["warningLines"].size(), GameEngine::Editor::kBuildStatusMaxWarningLines);
}

} // namespace
