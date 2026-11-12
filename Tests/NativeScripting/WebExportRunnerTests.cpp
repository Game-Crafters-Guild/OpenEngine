// The Web export's player template: the three files the WebPlayer link produces and the
// third-party notices its build writes beside them. A template without the notices is refused,
// because every exported game ships them.

#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/WebExportRunner.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

namespace
{

void WriteStubFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path).put('\n');
}

std::string RunExportWithTemplate(const std::filesystem::path& root, const std::filesystem::path& playerTemplate)
{
    BuildSettings settings;
    settings.projectRoot = root / "Project";
    settings.outputDirectory = root / "Out";
    // The export finds its script from the project root upward before it reads the template.
    WriteStubFile(settings.projectRoot / "Tools" / "Web" / "export_web_player.py");

    BuildPipeline pipeline(nullptr);
    WebExportInputs inputs;
    inputs.playerTemplate = playerTemplate;
    std::string error;
    EXPECT_FALSE(RunWebExport(settings, pipeline, inputs, {}, error));
    return error;
}

} // namespace

TEST(WebExportRunner, ATemplateWithoutTheNoticesIsRefusedAndTheErrorNamesTheBuild)
{
    TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("WebExportTemplate"));
    const std::filesystem::path playerTemplate = root.Path() / "Template";
    for (const char* name : {"WebPlayer.html", "WebPlayer.js", "WebPlayer.wasm"})
        WriteStubFile(playerTemplate / name);

    // The template exists, so the error names its folder and the missing file, not a missing template.
    const std::string refused = RunExportWithTemplate(root.Path(), playerTemplate);
    EXPECT_EQ(refused.find("No wasm Player template found"), std::string::npos) << refused;
    EXPECT_NE(refused.find(playerTemplate.generic_string() + " has no THIRD_PARTY_NOTICES.md"), std::string::npos)
        << refused;
    EXPECT_NE(refused.find("--target WebPlayer"), std::string::npos) << refused;

    // With the notices the template passes, and the export stops at a later input instead.
    WriteStubFile(playerTemplate / "THIRD_PARTY_NOTICES.md");
    const std::string accepted = RunExportWithTemplate(root.Path(), playerTemplate);
    EXPECT_EQ(accepted.find("No wasm Player template found"), std::string::npos) << accepted;
}
