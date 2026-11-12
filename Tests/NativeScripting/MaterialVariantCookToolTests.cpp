// The desktop export's material cook step (BuildPipeline::CookMaterialVariants): the cook's command
// line, the record it writes, and the copy of its packages into the bundle's read-only shader cache.

#include "Engine/Build/MaterialVariantCookTool.h"
#include "Rendering/ShaderCache/MetalLibraryBake.h"
#include "Rendering/ShaderCache/ShaderPackageContainer.h"
#include "Rendering/ShaderCache/ShaderStageRecord.h"
#include "Types/PathUtils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
namespace fs = std::filesystem;

namespace
{
// The record's file name and first line (MaterialCookRecord).
constexpr const char* kRecordFileName = "cook-record.txt";
constexpr std::string_view kRecordHeader = "material-cook-record";

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

TEST(MaterialVariantCookTool, TheCookRecordHoldsEachPackageOnceAndTheFailedVariants)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    MaterialCookRecord written;
    written.Packages = {cache / "bb" / "program.shaderpkg", cache / "aa" / "program.shaderpkg",
                        cache / "bb" / "program.shaderpkg",
                        cache / PathFromUtf8("Gr\xC3\xA4ser Feld") / "program.shaderpkg"};
    written.Failures = {{"Broken", "base", "surface 'Missing'\nnot found"}, {"Glass", "forward-color", ""}};
    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, written, error)) << error;

    const std::optional<MaterialCookRecord> read = ReadMaterialCookRecord(cache);
    ASSERT_TRUE(read);
    const std::vector<fs::path> expected = {written.Packages[0], written.Packages[1], written.Packages[3]};
    EXPECT_EQ(read->Packages, expected);
    ASSERT_EQ(read->Failures.size(), 2u);
    EXPECT_EQ(read->Failures[0].Material, "Broken");
    EXPECT_EQ(read->Failures[0].Variant, "base");
    EXPECT_EQ(read->Failures[0].Reason, "surface 'Missing' not found") << "a row is one line";
    EXPECT_EQ(read->Failures[1].Variant, "forward-color");

    // The writer ends each row with LF; a copy saved with CRLF reads the same.
    std::ifstream in(cache / kRecordFileName, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    EXPECT_EQ(text.find('\r'), std::string::npos);
    std::string crlf;
    for (const char c : text)
        crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    WriteFile(cache / kRecordFileName, crlf);
    const std::optional<MaterialCookRecord> readCrlf = ReadMaterialCookRecord(cache);
    ASSERT_TRUE(readCrlf);
    EXPECT_EQ(readCrlf->Packages, expected);
    EXPECT_EQ(readCrlf->Failures.size(), 2u);
}

TEST(MaterialVariantCookTool, AMissingCookRecordIsNotAnEmptyOne)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    EXPECT_FALSE(ReadMaterialCookRecord(cache)) << "a cook that wrote no record";

    // A write cut short leaves only its temporary sibling, which is not the record.
    WriteFile(cache / (std::string(kRecordFileName) + ".4242-0.tmp"),
              std::string(kRecordHeader) + "\npackage\t" + PathToUtf8(cache / "aa" / "program.shaderpkg") +
                  "\n");
    EXPECT_FALSE(ReadMaterialCookRecord(cache));
    const MaterialCookOutcome missing = ReadMaterialCookOutcome(cache, 0, "");
    EXPECT_FALSE(missing.Record);
    EXPECT_NE(missing.Error.find("MaterialVariantCook exited 0 and wrote no readable record"), std::string::npos)
        << missing.Error;
    EXPECT_NE(missing.Error.find("build the Editor target again"), std::string::npos) << missing.Error;

    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, {}, error)) << error;
    const MaterialCookOutcome empty = ReadMaterialCookOutcome(cache, 0, "");
    ASSERT_TRUE(empty.Record) << "a cook of no material";
    EXPECT_TRUE(empty.Record->Packages.empty());
    EXPECT_TRUE(empty.Error.empty()) << empty.Error;
    EXPECT_TRUE(empty.Warning.empty()) << empty.Warning;
}

// The writer replaces the record by a rename, so a file linked to the old record keeps its text.
TEST(MaterialVariantCookTool, TheCookRecordWriteReplacesTheFileAndDoesNotWriteIntoIt)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    WriteFile(cache / "earlier.txt", "earlier");
    fs::create_hard_link(cache / "earlier.txt", cache / kRecordFileName);
    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, {}, error)) << error;
    std::ifstream in(cache / "earlier.txt", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), "earlier");
}

TEST(MaterialVariantCookTool, ARecordThatIsNotUtf8IsNoRecord)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    WriteFile(cache / kRecordFileName,
              std::string(kRecordHeader) + "\npackage\t" + PathToUtf8(cache) + "/Gr\xE4ser/program.shaderpkg\n");
    EXPECT_FALSE(ReadMaterialCookRecord(cache));
}

// A failure text in a code page is written with U+FFFD for each byte that is not UTF-8, so the
// record keeps its packages.
TEST(MaterialVariantCookTool, AFailureThatIsNotUtf8KeepsTheRecordReadable)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    MaterialCookRecord record;
    record.Packages = {cache / "aa" / "program.shaderpkg"};
    record.Failures = {{"Gr\xE4ser", "base", "Zugriff verweigert: Gr\xE4ser"}};
    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, record, error)) << error;
    const std::optional<MaterialCookRecord> read = ReadMaterialCookRecord(cache);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->Packages, record.Packages);
    ASSERT_EQ(read->Failures.size(), 1u);
    EXPECT_EQ(read->Failures[0].Material, "Gr\xEF\xBF\xBDser");
    EXPECT_EQ(read->Failures[0].Reason, "Zugriff verweigert: Gr\xEF\xBF\xBDser");
}

TEST(MaterialVariantCookTool, TheWarningNamesTheFailedMaterialsOfTheRecord)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    MaterialCookRecord record;
    record.Packages = {cache / "aa" / "program.shaderpkg"};
    record.Failures = {{"Broken", "base", "surface 'Missing' not found"},
                       {"Glass", "forward-color", "WGSL: naga refused it"},
                       {"Broken", "forward-color-scene-grab", ""},
                       {"Broken", "base", ""}};
    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, record, error)) << error;

    // The console row of Glass is behind log text, so a reader of the console would not see it.
    const MaterialCookOutcome outcome =
        ReadMaterialCookOutcome(cache, 1, "[info] Shader cache: probe  FAIL Glass [forward-color]\n");
    EXPECT_TRUE(outcome.Error.empty()) << outcome.Error;
    EXPECT_NE(outcome.Warning.find("these materials compile when the game first draws them:\n"
                                   "  Broken: base, forward-color-scene-grab\n"
                                   "  Glass: forward-color"),
              std::string::npos)
        << outcome.Warning;
}

// A material that did not load is not compiled on first use: the warning names its path and error and
// states the fix, also beside failed variants.
TEST(MaterialVariantCookTool, TheWarningNamesAMaterialThatDidNotLoadAndItsFix)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    MaterialCookRecord record;
    record.Packages = {cache / "aa" / "program.shaderpkg"};
    record.Failures = {{"C:/Game/Assets/Broken.material", "", "JSON parse error at line 1"}};
    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, record, error)) << error;
    const MaterialCookOutcome unloaded = ReadMaterialCookOutcome(cache, 1, "");
    EXPECT_EQ(unloaded.Warning, "MaterialVariantCook failed (exit 1); the cook could not load or prepare these "
                                "materials. Fix each material, then export again:\n  C:/Game/Assets/Broken.material: "
                                "JSON parse error at line 1");

    record.Failures.push_back({"Glass", "forward-color", "WGSL: naga refused it"});
    ASSERT_TRUE(WriteMaterialCookRecord(cache, record, error)) << error;
    const MaterialCookOutcome both = ReadMaterialCookOutcome(cache, 1, "");
    EXPECT_EQ(both.Warning, unloaded.Warning + "\nThese materials compile when the game first draws them:\n"
                                               "  Glass: forward-color");
}

TEST(MaterialVariantCookTool, ACookThatStopsEarlyLeavesNothingToStage)
{
    TempTree tree;
    const fs::path cache = tree.Root / "Cook";
    std::string error;
    ASSERT_TRUE(WriteMaterialCookRecord(cache, {{cache / "aa" / "program.shaderpkg"}, {}}, error)) << error;
    WriteFile(cache / "program-index.bin", "index of an earlier cook");
    // The temporary sibling of a write whose process is gone (no process has this id).
    const fs::path orphan = cache / (std::string(kRecordFileName) + ".2147483600-77-0.tmp");
    WriteFile(orphan, "cut");

    RemoveMaterialCookOutputs(cache);
    EXPECT_FALSE(fs::exists(cache / kRecordFileName));
    EXPECT_FALSE(fs::exists(cache / "program-index.bin"));
    EXPECT_FALSE(fs::exists(orphan));
    // The cook then stops on its arguments (exit 2) and writes no record.
    const MaterialCookOutcome outcome = ReadMaterialCookOutcome(cache, 2, "MaterialVariantCook: unknown option\n");
    EXPECT_FALSE(outcome.Record);
    EXPECT_TRUE(outcome.Error.empty()) << outcome.Error;
    EXPECT_NE(outcome.Warning.find("the export ships no cooked shader package"), std::string::npos)
        << outcome.Warning;
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
    EXPECT_NE(error.find(PathToUtf8(cache / "missing" / "program.shaderpkg") + "', which is not on disk"),
              std::string::npos)
        << error;
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

#if defined(__APPLE__)
// A Mac export compiles a stage that a development run recorded from an engine shader package: the
// export stages those packages under Assets/Shaders, and the bake finds them through the cook's folders.
TEST(MaterialVariantCookTool, RecordedEngineStageBakesFromTheExportsShaderRoots)
{
    // An empty compute kernel (local size 8x1x1) as SPIR-V words.
    constexpr uint32_t kKernel[] = {0x07230203, 0x00010000, 0, 5,  0,          0x00020011, 1,          0x0003000E,
                                    0,          1,          0x0005000F, 5, 1, 0x6E69616D, 0,          0x00060010,
                                    1,          17,         8,          1, 1, 0x00020013, 2,          0x00030021,
                                    3,          2,          0x00050036, 2, 1, 0,          3,          0x000200F8,
                                    4,          0x000100FD, 0x00010038};
    const auto* first = reinterpret_cast<const uint8_t*>(kKernel);
    const std::vector<uint8_t> spirv(first, first + sizeof(kKernel));
    TempTree tree;
    std::vector<uint8_t> package;
    const Rendering::ShaderPackageChunkSource chunk{"cs-spv", spirv};
    ASSERT_TRUE(Rendering::WriteShaderPackageContainer(std::span(&chunk, 1), package));
    WriteFile(tree.Root / "Staging/Assets/Shaders/frustum_culling.shaderpkg", std::string(package.begin(), package.end()));
    WriteFile(tree.Root / "ShaderStages.txt",
              Rendering::FormatShaderStageRecord(Rendering::MakeShaderStageRecord(spirv, "comp", {}, nullptr)) + "\n");

    DesktopMaterialCookInputs inputs;
    inputs.EngineShaders = tree.Root / "Staging/Assets/Shaders";
    inputs.PackageShaderDirs = {inputs.EngineShaders / "GPUFogParticles"};
    inputs.Cache = tree.Root / "Game/.Build/MaterialVariantCook";
    const std::vector<fs::path> roots = DesktopShaderPackageRoots(inputs);
    EXPECT_EQ(roots, (std::vector<fs::path>{inputs.EngineShaders, inputs.Cache}));
    const Rendering::MetalLibraryBake bake = Rendering::BakeMetalLibraries(
        tree.Root / "ShaderStages.txt", roots, tree.Root / "Work",
        [](std::span<const Rendering::MetalLibraryCompileJob> jobs, std::span<const std::string>) {
            for (const Rendering::MetalLibraryCompileJob& job : jobs)
                std::ofstream(job.Library, std::ios::binary) << "stand-in";
        });
    EXPECT_EQ(bake.ResolvedStages, 1u);
    EXPECT_EQ(bake.CompiledStages, 1u);
}
#endif
