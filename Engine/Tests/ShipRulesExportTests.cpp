// Ship rules on the export routes that run BuildPipeline::Execute: the Linux and Steam Deck
// runtime-template routes, which run on any host. An Always folder ships what nothing uses, a
// Never asset that a shipped asset uses fails the export before any copy, and so does a Never
// asset that a build step adds.

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/RuntimeHumanoidProfile.h"
#include "Core/Engine.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/BuildPipeline.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{

void WriteFile(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The target name is the parameter: both routes share step 6, and the test guards that.
class ShipRulesExport : public testing::TestWithParam<const char*>
{
protected:
    void SetUp() override
    {
        WriteFile(Assets / "Scenes/Main.scene", "[scene name=\"Main\" version=1]\n");
        WriteFile(Assets / "Data/.assetfolder", R"({"ship": "always"})");
        WriteFile(Assets / "Data/units.json", "{}");
        WriteFile(Assets / "Loose/notes.json", "{}");
        WriteFile(Assets / "Old/.assetfolder", R"({"ship": "never"})");
        WriteFile(Assets / "Old/units.json", "{}");
        // Opaque mandatory dependencies; this fixture does not render text or retarget a rig.
        WriteFile(Assets / "Fonts/Roboto-Regular.ttf", "TTFDATA");
        WriteFile(Assets / kRuntimeHumanoidProfilePath, "{}");
        // The public prebuilt-runtime route stages this header without launching it.
        std::array<char, 20> elf{};
        elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
        elf[4] = 2; elf[5] = 1; elf[18] = 62;
        WriteFile(Runtime / "Player", {elf.data(), elf.size()});
    }
    void TearDown() override
    {
        Engine.Shutdown();
        fs::current_path(PreviousDirectory);
    }
    void Start()
    {
        Config.WorkspaceDirectory = Project.string();
        Config.AssetDirectory = Assets.string();
        Config.AssetDatabaseFile = (Project / "AssetDatabase.assetdb").string();
        Config.AssetDatabaseCacheDirectory = (Project / ".Cache/AssetDatabase").string();
        ASSERT_TRUE(Engine.Initialize(Config));
        Engine.GetAssetManager().WaitForStartupScan();
    }
    BuildSettings Settings() const
    {
        BuildSettings build;
        build.platformName = GetParam();
        build.projectRoot = Project;
        build.prebuiltPlayerDirectory = Runtime;
        build.outputDirectory = Output;
        build.compileScripts = false;
        build.buildConfiguration = "Release";
        build.scenes = {"Scenes/Main.scene"};
        build.playerConfig.gameName = "ShipRulesProbe";
        return build;
    }
    static bool HasError(const BuildProgress& progress, const std::string& sentence)
    {
        return std::find(progress.errors.begin(), progress.errors.end(), sentence) != progress.errors.end();
    }
    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("ship_rules_export")};
    const fs::path PreviousDirectory = fs::current_path();
    const fs::path Project = Temporary.Path() / "Project";
    const fs::path Assets = Project / "Assets";
    const fs::path Runtime = Temporary.Path() / "Runtime";
    const fs::path Output = Temporary.Path() / "Export";
    ApplicationConfig Config;
    EngineCore Engine;
};

} // namespace

TEST_P(ShipRulesExport, AnAlwaysFolderShipsWhatNothingUses)
{
    Start();
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(Settings(), [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    EXPECT_TRUE(fs::exists(Output / "Assets/Data/units.json"));
    EXPECT_FALSE(fs::exists(Output / "Assets/Data/.assetfolder"));
    EXPECT_FALSE(fs::exists(Output / "Assets/Loose/notes.json"));
    EXPECT_FALSE(fs::exists(Output / "Assets/Old/units.json"));
}

TEST_P(ShipRulesExport, AnAssetAlwaysShipsUnreferencedAndAnAssetNeverStaysOutOfItsAlwaysFolder)
{
    Start();
    AssetRegistry& registry = Engine.GetAssetManager().GetRegistry();
    const std::string key(ShipRules::kAssetMetaKey);
    ASSERT_TRUE(registry.SetMetaValue(Assets / "Loose/notes.json", key, "always"));
    ASSERT_TRUE(registry.SetMetaValue(Assets / "Data/units.json", key, "never"));
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    ASSERT_TRUE(pipeline.Execute(Settings(), [&progress](const BuildProgress& current) { progress = current; }))
        << progress.statusMessage << '\n' << testing::PrintToString(progress.errors);
    EXPECT_TRUE(fs::exists(Output / "Assets/Loose/notes.json"));
    EXPECT_FALSE(fs::exists(Output / "Assets/Data/units.json"));
    const std::string keptOut =
        "Data/units.json is set to Never ship (from its own rule), so it does not ship, although its folder Data/ is "
        "Always.";
    EXPECT_NE(std::find(progress.warnings.begin(), progress.warnings.end(), keptOut), progress.warnings.end())
        << testing::PrintToString(progress.warnings);
}

TEST_P(ShipRulesExport, ANeverAssetAShippedSceneUsesFailsBeforeAnyCopy)
{
    WriteFile(Assets / "Old/Wall.png", "PNGDATA");
    WriteFile(Assets / "Scenes/Main.scene", "[scene name=\"Main\" version=1]\n"
                                            "[resource id=\"wall\" path=\"Old/Wall.png\"]\n"
                                            "\n"
                                            "[entity id=\"e_1\"]\n"
                                            "MeshRenderer.material = #wall\n");
    Start();
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    EXPECT_FALSE(pipeline.Execute(Settings(), [&progress](const BuildProgress& current) { progress = current; }));
    EXPECT_TRUE(HasError(progress, "Old/Wall.png is set to Never ship (from its folder Old/), but a shipped asset "
                                   "uses it: Scenes/Main.scene → Old/Wall.png. Remove the reference, or change the "
                                   "rule."))
        << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets"));
}

TEST_P(ShipRulesExport, ABuildStepThatAddsANeverAssetFailsTheExport)
{
    Start();
    BuildPipeline pipeline(nullptr);
    BuildStep addOld;
    addOld.name = "add-old-data";
    addOld.phase = BuildStep::Phase::PreAssetCopy;
    const fs::path oldData = Assets / "Old/units.json";
    addOld.execute = [oldData](BuildContext& context) {
        AssetManifestEntry entry;
        entry.sourcePath = oldData;
        entry.outputPath = "Assets/Old/units.json";
        context.manifest.entries.push_back(entry);
        return true;
    };
    pipeline.RegisterStep(std::move(addOld));
    BuildProgress progress;
    EXPECT_FALSE(pipeline.Execute(Settings(), [&progress](const BuildProgress& current) { progress = current; }));
    EXPECT_TRUE(HasError(progress, "Old/units.json is set to Never ship (from its folder Old/), but a build step "
                                   "added it. Change the step or the rule."))
        << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets"));
}

TEST_P(ShipRulesExport, ANeverEngineDefaultFailsTheExport)
{
    // The default UI font is a project asset in this fixture. With no editor mount, the export
    // takes the UI control assets from the project too.
    WriteFile(Assets / "Fonts/.assetfolder", R"({"ship": "never"})");
    WriteFile(Assets / "UI/controls/Button.css", ".button { width: 1px; }\n");
    WriteFile(Assets / "UI/controls/.assetfolder", R"({"ship": "never"})");
    Start();
    BuildPipeline pipeline(nullptr);
    BuildProgress progress;
    EXPECT_FALSE(pipeline.Execute(Settings(), [&progress](const BuildProgress& current) { progress = current; }));
    EXPECT_TRUE(HasError(progress, "Fonts/Roboto-Regular.ttf is set to Never ship (from its folder Fonts/), but every "
                                   "game ships it as an engine default (the render pipeline, the runtime humanoid "
                                   "profile, the default UI font or the UI control assets). Change the rule."))
        << testing::PrintToString(progress.errors);
    EXPECT_TRUE(HasError(progress, "UI/controls/Button.css is set to Never ship (from its folder UI/controls/), but every "
                                   "game ships it as an engine default (the render pipeline, the runtime humanoid "
                                   "profile, the default UI font or the UI control assets). Change the rule."))
        << testing::PrintToString(progress.errors);
    EXPECT_FALSE(fs::exists(Output / "Assets"));
}

INSTANTIATE_TEST_SUITE_P(TemplateRoutes, ShipRulesExport, testing::Values("Linux", "Steam"));
