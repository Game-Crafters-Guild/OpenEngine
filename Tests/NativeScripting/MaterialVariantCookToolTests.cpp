// The desktop export's material cook step (BuildPipeline::CookMaterialVariants): the cook's command
// line, the packages its output names, and their copy into the bundle's read-only shader cache.

#include "Engine/Build/MaterialVariantCookTool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{
struct TempTree
{
    fs::path Root;
    TempTree()
    {
        static std::atomic<uint32_t> counter{0};
        Root = fs::temp_directory_path() / ("ge_material_cook_tool_" +
                                            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                                            std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

void WriteFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
} // namespace

TEST(MaterialVariantCookTool, DesktopArgumentsNameEveryRootAndVerify)
{
    DesktopMaterialCookInputs inputs;
    inputs.ProjectAssets = "Game/Assets";
    inputs.EngineShaders = "Staging/Assets/Shaders";
    inputs.PackageShaderDirs = {"Packages/trees/Assets/Shaders", "Staging/Assets/Shaders/GPUFogParticles"};
    inputs.ScanRoots = {{"Packages/trees/Assets", "trees"}};
    inputs.Cache = "Game/.Build/MaterialVariantCook";
    inputs.ReuseCache = "Game/.Cache/Shaders";
    const std::vector<std::string> expected = {
        "--project", fs::path("Game/Assets").string(),
        "--engine-shaders", fs::path("Staging/Assets/Shaders").string(),
        "--cache", fs::path("Game/.Build/MaterialVariantCook").string(),
        "--verify",
        "--package-shaders", fs::path("Packages/trees/Assets/Shaders").string(),
        "--package-shaders", fs::path("Staging/Assets/Shaders/GPUFogParticles").string(),
        "--scan", fs::path("Packages/trees/Assets").string(), "--scan-alias", "trees",
        "--reuse-cache", fs::path("Game/.Cache/Shaders").string()};
    EXPECT_EQ(DesktopMaterialCookArguments(inputs), expected);
    for (const std::string& argument : DesktopMaterialCookArguments(inputs))
        EXPECT_NE(argument, "--web") << "a desktop export cooks the desktop profile";
}

TEST(MaterialVariantCookTool, CookedPackagesAreTheOkLinesEachOnce)
{
    const std::string output = "[INFO] compiling\n"
                               "  ok   Rock [base] -> /cache/aa/program.shaderpkg\n"
                               "  ok   Rock [forward-color] -> /cache/bb/program.shaderpkg\r\n"
                               "  FAIL Glass [base]\n"
                               "        an error naming ] -> /cache/not/a/package\n"
                               "  ok   engine-default-pbr [base] -> /cache/aa/program.shaderpkg\n"
                               "MaterialVariantCook: 3 variant(s) cooked";
    const std::vector<fs::path> expected = {"/cache/aa/program.shaderpkg", "/cache/bb/program.shaderpkg"};
    EXPECT_EQ(CookedShaderPackages(output), expected);
    EXPECT_TRUE(CookedShaderPackages("").empty());
}

TEST(MaterialVariantCookTool, FailedMaterialsAreTheFailLinesGroupedByMaterial)
{
    const std::string output = "  ok   Rock [base] -> /cache/aa/program.shaderpkg\n"
                               "  FAIL Broken [base]\n"
                               "        Broken.material: surface 'Missing' not found\n"
                               "  FAIL Glass [forward-color]\r\n"
                               "  FAIL Broken [forward-color-scene-grab]\n"
                               "  FAIL Broken [base]\n"
                               "MaterialVariantCook: 4 variant(s) failed";
    const std::vector<std::string> expected = {"Broken: base, forward-color-scene-grab", "Glass: forward-color"};
    EXPECT_EQ(FailedCookMaterials(output), expected);
    EXPECT_TRUE(FailedCookMaterials("  ok   Rock [base] -> /cache/aa/program.shaderpkg").empty());
}

TEST(MaterialVariantCookTool, StagingCopiesEachPackageUnderItsKeyAndNothingElse)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    WriteFile(cache / "aa" / "program.shaderpkg", "first");
    WriteFile(cache / "bb" / "program.shaderpkg", "second");
    WriteFile(cache / "cc" / "program.shaderpkg", "stale, not named by this cook");
    WriteFile(cache / "Generated" / "Materials" / "surface.glsl", "derived source");
    const fs::path bundle = tree.Root / "Game.app" / "Contents" / "Resources" / ".Cache" / "Shaders";

    std::string error;
    ASSERT_TRUE(StageCookedShaderPackages({cache / "aa" / "program.shaderpkg", cache / "bb" / "program.shaderpkg"},
                                          cache, bundle, error))
        << error;
    EXPECT_TRUE(fs::is_regular_file(bundle / "aa" / "program.shaderpkg"));
    EXPECT_TRUE(fs::is_regular_file(bundle / "bb" / "program.shaderpkg"));
    EXPECT_FALSE(fs::exists(bundle / "cc"));
    EXPECT_FALSE(fs::exists(bundle / "Generated"));

    EXPECT_FALSE(StageCookedShaderPackages({tree.Root / "Elsewhere" / "program.shaderpkg"}, cache, bundle, error));
    EXPECT_NE(error.find("outside its cache"), std::string::npos) << error;
    EXPECT_FALSE(StageCookedShaderPackages({cache / "missing" / "program.shaderpkg"}, cache, bundle, error));
    EXPECT_NE(error.find("Could not copy"), std::string::npos) << error;
}

TEST(MaterialVariantCookTool, StagingShipsTheProgramIndexBesideThePackages)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    WriteFile(cache / "aa" / "program.shaderpkg", "first");
    WriteFile(cache / "program-index.bin", "index");
    const fs::path bundle = tree.Root / "Game" / ".Cache" / "Shaders";

    std::string error;
    ASSERT_TRUE(StageCookedShaderPackages({cache / "aa" / "program.shaderpkg"}, cache, bundle, error)) << error;
    EXPECT_TRUE(fs::is_regular_file(bundle / "aa" / "program.shaderpkg"));
    EXPECT_TRUE(fs::is_regular_file(bundle / "program-index.bin"))
        << "the runtime finds its cooked variants through the index, so it ships with them";
}
