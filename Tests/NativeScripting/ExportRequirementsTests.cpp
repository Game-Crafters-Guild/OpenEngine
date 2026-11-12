// The tools an export needs on the build machine: a check runs each command of a requirement, and the
// warning of an export without it names what the export does instead and the install command.

#include "Engine/Build/ExportRequirements.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine;

TEST(ExportRequirements, MetalToolchainIsInstalledOnlyWhenEachToolRuns)
{
    const ExportRequirement& metal = MetalToolchainRequirement();
    std::vector<std::string> ran;
    EXPECT_TRUE(IsExportRequirementInstalled(metal, [&](const std::string& command) {
        ran.push_back(command);
        return 0;
    }));
    EXPECT_EQ(ran, (std::vector<std::string>{"xcrun -sdk macosx metal --version", "xcrun -sdk macosx metallib --version"}));
    // Without the toolchain `xcrun` still finds stubs, so each tool must run.
    EXPECT_FALSE(IsExportRequirementInstalled(
        metal, [](const std::string& command) { return command.find("metallib") == std::string::npos ? 0 : 1; }));
}

TEST(ExportRequirements, MissingMetalToolchainNamesTheFallbackAndTheInstallCommand)
{
    const std::string warning = DescribeMissingExportRequirement(MetalToolchainRequirement());
    EXPECT_NE(warning.find("'xcodebuild -downloadComponent MetalToolchain'"), std::string::npos) << warning;
    EXPECT_NE(warning.find("compiles each shader stage when it first draws"), std::string::npos) << warning;
}

TEST(ExportRequirements, OnlyAMacListsTheMetalToolchainForAMacExport)
{
#if defined(__APPLE__)
    EXPECT_EQ(ExportRequirementsForPlatform("Mac"), (std::vector<const ExportRequirement*>{&MetalToolchainRequirement()}));
#else
    EXPECT_TRUE(ExportRequirementsForPlatform("Mac").empty());
#endif
    EXPECT_TRUE(ExportRequirementsForPlatform("Windows").empty());
}
