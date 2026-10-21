#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Packages/PackageMounts.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/MaterialVariantCookTool.h"
#include "Engine/Rendering/MaterialPrewarmVariants.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialShaderPackageBuilder.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "StagedTestPaths.h"
#include "TerrainGrass/GrassComputePrograms.h"
#include "TestTempDir.h"
#include "MeshVariantTable.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

void CopyGraphSamples(const fs::path& project)
{
    const fs::path source = GameEngine::TestPaths::StagedRoot() / "Assets/Materials/Graph";
    ASSERT_TRUE(fs::is_regular_file(source / "GraphSamplePBR.material"));
    ASSERT_TRUE(fs::is_regular_file(source / "PreviewSphereTest.material"));
    const fs::path graphs = project / "Assets/Materials/Graph";
    fs::create_directories(graphs);
    fs::copy(source, graphs, fs::copy_options::recursive);
}

// The repository's script, as the web editor's cook uses: setup.py provisions naga
// and tint in the toolchain directory beside it, where the script looks first.
fs::path ShaderCookScript()
{
    return fs::path(GE_RENDERER_REPO_ROOT) / "Tools/ShaderCook/shadercook.py";
}

GameEngine::ShellProcessResult Cook(const fs::path& project, bool web = false,
                                    const std::vector<std::string>& extraArguments = {})
{
    const fs::path fixtures = GameEngine::TestPaths::StagedRoot();
    const fs::path executableDirectory = GameEngine::TestPaths::ExecutableDirectory();
    const fs::path tool = executableDirectory.parent_path() / "Tools" /
                          GE_MATERIAL_VARIANT_COOK_FILENAME;
    std::vector<std::string> arguments{
        "--project", (project / "Assets").string(),
        "--engine-shaders", (fixtures / "Engine/Modules/Rendering/Shaders").string(),
        "--package-shaders", (fixtures / "Engine/Modules/GPUFogParticles/Shaders").string(),
        "--package-shaders", (executableDirectory /
            "Fixtures/MaterialVariantCook/PackageShaders").string(),
        "--cache", (project / ".Cache/Shaders").string(), "--verify"};
    arguments.insert(arguments.end(), extraArguments.begin(), extraArguments.end());
    if (web)
    {
        arguments.emplace_back("--web");
        arguments.emplace_back("--shadercook");
        arguments.push_back(ShaderCookScript().string());
    }
    // The web cook translates every built-in target; on a loaded host it takes 25 minutes.
    return GameEngine::RunProcessCaptured(tool.string(), arguments, std::chrono::minutes(45));
}

std::size_t CookedVariantCount(const std::string& output, const std::string& material)
{
    const std::string prefix = "  ok   " + material + " [";
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = output.find(prefix, position)) != std::string::npos)
    {
        ++count;
        position += prefix.size();
    }
    return count;
}

// The rows `material` cooked whose row name carries the vertex-colour stream ("+color").
std::size_t CookedVertexColorVariantCount(const std::string& output, const std::string& material)
{
    const std::string prefix = "  ok   " + material + " [";
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = output.find(prefix, position)) != std::string::npos)
    {
        position += prefix.size();
        const std::string row = output.substr(position, output.find(']', position) - position);
        count += row.find("+color") != std::string::npos ? 1u : 0u;
    }
    return count;
}

// A cook that verified, and the table it cooked: the imported mesh materials (the built-in default PBR)
// with their vertex-colour rows, a project's materials with the same table less those rows, and no
// particle shape with any.
void CheckCook(const GameEngine::ShellProcessResult& result)
{
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("verify: every cooked variant resolves with no compiler"),
              std::string::npos) << result.output;
    const auto controlCount = CookedVariantCount(result.output, "engine-default-pbr");
    const auto controlColorCount = CookedVertexColorVariantCount(result.output, "engine-default-pbr");
    ASSERT_GT(controlCount, 0u) << result.output;
    EXPECT_GT(controlColorCount, 0u) << "the default material cooks the vertex-colour rows\n" << result.output;
    for (const char* sample : {"GraphSamplePBR", "PreviewSphereTest"})
        EXPECT_EQ(CookedVariantCount(result.output, sample), controlCount - controlColorCount)
            << sample << '\n' << result.output;
    const std::size_t shapes = GameEngine::Particles::ParticleRenderMaterialShapes().size();
    for (std::size_t shape = 0; shape < shapes; ++shape)
        EXPECT_EQ(CookedVertexColorVariantCount(result.output, "particles-" + std::to_string(shape)), 0u)
            << "particles-" << shape << " cooks no vertex-colour row\n" << result.output;
}

std::set<std::string> FileNames(const fs::path& directory)
{
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(directory))
        names.insert(entry.path().filename().string());
    return names;
}

// A project whose one material binds a height map on the standard surface, which is the whole opt-in
// to the relief march; the map's GUID need not resolve for the program to compose.
void WriteReliefMaterial(const fs::path& project)
{
    const fs::path materials = project / "Assets/Materials";
    fs::create_directories(materials);
    std::ofstream(materials / "Relief.material") << R"({
  "schemaVersion": 3,
  "materialName": "Relief",
  "lightingModel": "StandardPBR",
  "alphaMode": "Opaque",
  "surfaceShader": "Surfaces/standard_pbr.glsl",
  "textures": {
    "heightMap": {"guid": "69a5e97a-f000-40f6-924c-c14f53956ed3", "path": "Textures/height.png"}
  }
})";
}

// Each cooked variant of `material` with its package, from the cook's "  ok   <material> [<variant>]
// -> <package>" lines.
std::vector<std::pair<std::string, fs::path>> CookedPackages(const std::string& output, const std::string& material)
{
    std::vector<std::pair<std::string, fs::path>> packages;
    const std::string prefix = "  ok   " + material + " [";
    const std::string arrow = "] -> ";
    std::size_t position = 0;
    while ((position = output.find(prefix, position)) != std::string::npos)
    {
        const std::size_t variantStart = position + prefix.size();
        const std::size_t arrowAt = output.find(arrow, variantStart);
        const std::size_t lineEnd = output.find_first_of("\r\n", variantStart);
        if (arrowAt == std::string::npos || arrowAt > lineEnd)
            break;
        packages.emplace_back(output.substr(variantStart, arrowAt - variantStart),
                              fs::path(output.substr(arrowAt + arrow.size(), lineEnd - arrowAt - arrow.size())));
        position = lineEnd;
    }
    return packages;
}

// SPIR-V's OpCapability, and the capability interpolateAtOffset needs (gated on sampleRateShading).
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr std::size_t kSpirvHeaderWords = 5;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kCapabilityInterpolationFunction = 52;

// Whether a SPIR-V module declares `capability`; capabilities come first after the header.
bool DeclaresCapability(const std::vector<uint8_t>& bytes, uint32_t capability)
{
    if (bytes.size() % sizeof(uint32_t) != 0 || bytes.size() < kSpirvHeaderWords * sizeof(uint32_t))
        return false;
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] != kSpirvMagic)
        return false;
    for (std::size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const uint32_t opcode = words[at] & 0xFFFFu;
        const uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size())
            return false;
        if (opcode == kOpCapability && count == 2 && words[at + 1] == capability)
            return true;
        at += count;
    }
    return false;
}

// Two materials whose lobes the runtime compiles into every program it asks for: a clear coat, and a
// transmissive glass, which the transmissive pass also draws with the scene-colour grab bound.
void WriteLobeMaterials(const fs::path& project)
{
    const fs::path materials = project / "Assets/Materials";
    fs::create_directories(materials);
    std::ofstream(materials / "ClearCoat.material") << R"({
  "schemaVersion": 3,
  "materialName": "ClearCoat",
  "lightingModel": "StandardPBR",
  "alphaMode": "Opaque",
  "surfaceShader": "Surfaces/standard_pbr.glsl",
  "properties": {"enableClearCoat": true, "clearCoat": 1.0, "clearCoatRoughness": 0.1}
})";
    std::ofstream(materials / "Glass.material") << R"({
  "schemaVersion": 3,
  "materialName": "Glass",
  "lightingModel": "StandardPBR",
  "alphaMode": "Opaque",
  "surfaceShader": "Surfaces/standard_pbr.glsl",
  "properties": {"enableTransmission": true, "transmissionWeight": 1.0}
})";
}

// The world passes the shipped render graphs draw a material with: ForwardPlus.rendergraph's, and the one
// without IBL that Web.rendergraph draws and prewarm asks for before a graph is applied.
constexpr GameEngine::Rendering::MaterialKeyword kWebWorldPass =
    GameEngine::Rendering::MaterialKeyword::Instanced | GameEngine::Rendering::MaterialKeyword::ForwardPlus |
    GameEngine::Rendering::MaterialKeyword::Shadows;
constexpr GameEngine::Rendering::MaterialKeyword kForwardPlusWorldPass =
    kWebWorldPass | GameEngine::Rendering::MaterialKeyword::IBL;

// The world passes a material's colour pass is drawn with: the shipped graphs' (kForwardPlusWorldPass,
// kWebWorldPass), and each world pass the mesh table declares for a view's per-view keywords (GTAO,
// SSSR, DDGI), with and without contact shadows. The colour pass draws a crossfading tail under every
// one of them.
std::vector<GameEngine::Rendering::MaterialKeyword> ColourWorldPasses()
{
    using GameEngine::Rendering::MaterialKeyword;
    std::vector<MaterialKeyword> passes = {kForwardPlusWorldPass, kWebWorldPass};
    for (const auto& row : GameEngine::Tools::MaterialCook::kMeshVariants)
        if (GameEngine::Rendering::HasKeyword(row.PassKeywords, MaterialKeyword::ForwardPlus))
            for (const MaterialKeyword pass : {row.PassKeywords, row.PassKeywords | MaterialKeyword::ScreenSpaceShadows})
                if (std::find(passes.begin(), passes.end(), pass) == passes.end())
                    passes.push_back(pass);
    return passes;
}

// Every key the runtime's prewarm asks for when it registers the material at `materialPath`
// (MaterialPrewarmVariantKeys), under every world pass the colour pass draws with (ColourWorldPasses):
// the base program, the colour, asset preview and depth programs with their LOD-crossfade tails, and
// the transmissive pass's grab variants. The transmissive pass draws with its graph node's keywords
// (TransmissivePassNode), a shipped graph's world pass, so only those ask for a grab variant. The
// registration key is the document's keywords on the layout registration infers from its asset.
std::vector<GameEngine::Rendering::ShaderVariantKey> RuntimePrewarmKeys(const GameEngine::MaterialDocument& document,
                                                                        const fs::path& materialPath)
{
    using GameEngine::Rendering::MaterialKeyword;
    GameEngine::Rendering::ShaderVariantKey base = GameEngine::Engine::Renderer::DeriveBaseVariantKey(document);
    base.vertexFlags = GameEngine::Engine::Renderer::InferVertexAttributeFlagsFromDocument(document, materialPath);
    std::vector<GameEngine::Rendering::ShaderVariantKey> keys;
    for (const MaterialKeyword worldPass : ColourWorldPasses())
    {
        const bool graphPass = worldPass == kForwardPlusWorldPass || worldPass == kWebWorldPass;
        for (const auto& key : GameEngine::Engine::Renderer::MaterialPrewarmVariantKeys(
                 base, worldPass, document.customVertexShader,
                 GameEngine::Engine::Renderer::DeriveRenderedAlphaMode(document), document.ignoreVertexColor))
            if ((graphPass || !GameEngine::Rendering::HasKeyword(key.materialKeywords, MaterialKeyword::SceneColorGrab)) &&
                std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(key);
    }
    return keys;
}

// The compile inputs the desktop cook keys its rows at, as a desktop device sets them: the full
// profile and SPIR-V 1.6. Restores the target on every exit.
struct ScopedDesktopCompileInputs
{
    using Target = GameEngine::Rendering::ShaderCompileService::SpirvTarget;
    ScopedDesktopCompileInputs() : Previous(GameEngine::Rendering::ShaderCompileService::MaxSupportedSpirv())
    {
        GameEngine::Rendering::ApplyShaderCompileProfile(GameEngine::Rendering::RendererProfile{});
        GameEngine::Rendering::ShaderCompileService::SetMaxSupportedSpirv(Target::Spirv_1_6);
    }
    ~ScopedDesktopCompileInputs() { GameEngine::Rendering::ShaderCompileService::SetMaxSupportedSpirv(Previous); }
    Target Previous;
};

// Restores the compiler on every exit, including a failed assertion.
struct ScopedCookOnlyRuntime
{
    ScopedCookOnlyRuntime() { GameEngine::Rendering::ShaderCompileService::SetCompilerAvailable(false); }
    ~ScopedCookOnlyRuntime() { GameEngine::Rendering::ShaderCompileService::SetCompilerAvailable(true); }
};

// Expects every key in `keys` of the material at `materialPath` to build from `bundle`, without a
// compiler, as the runtime's ShaderCompilationCache builds it: the compile spec's document, the key's
// keywords and layout.
void ExpectServedFromBundle(const GameEngine::MaterialDocument& document, const fs::path& materialPath,
                            const std::string& name, const std::vector<GameEngine::Rendering::ShaderVariantKey>& keys,
                            GameEngine::Rendering::MaterialBuildContext& context, const fs::path& bundle)
{
    using namespace GameEngine;
    const Engine::Renderer::MaterialCompileSpec spec = Engine::Renderer::DeriveCompileSpec(document);
    MaterialDocument runtimeDocument{};
    runtimeDocument.surfaceShader = spec.surfaceShaderPath;
    runtimeDocument.lightingModel = spec.lightingModel;
    runtimeDocument.vertexModifier = spec.vertexModifierPath;
    runtimeDocument.customVertexShader = spec.customVertexShader;
    runtimeDocument.keywords = spec.userKeywords;
    if (spec.alphaTest)
        runtimeDocument.alphaMode = MaterialAlphaMode::Mask;
    for (const auto& key : keys)
    {
        const Engine::Renderer::MaterialShaderPackageBuilder builder(runtimeDocument, materialPath, name, context);
        const Rendering::MaterialBuildResult served =
            builder.Build(Rendering::ShaderSourceKind::SpirV, key.materialKeywords, key.vertexFlags);
        EXPECT_TRUE(served.success)
            << name << " keywords 0x" << std::hex << static_cast<uint64_t>(key.materialKeywords) << " layout 0x"
            << static_cast<uint32_t>(key.vertexFlags) << std::dec
            << " was not cooked: " << (served.errors.empty() ? std::string{} : served.errors.front());
        EXPECT_EQ(fs::path(served.generatedShaderPkgPath).parent_path().parent_path(), bundle)
            << name << " was served from " << served.generatedShaderPkgPath << ", not from the bundle";
    }
}

// The cook's per-variant report lines, "  ok   ..." and "  FAIL ..." without their detail lines, in
// output order.
std::vector<std::string> ReportLines(const std::string& output)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < output.size())
    {
        std::size_t end = output.find_first_of("\r\n", start);
        if (end == std::string::npos)
            end = output.size();
        const std::string line = output.substr(start, end - start);
        if (line.starts_with("  ok   ") || line.starts_with("  FAIL "))
            lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::map<std::string, fs::file_time_type> PackageWriteTimes(const fs::path& cache)
{
    std::map<std::string, fs::file_time_type> result;
    for (const auto& entry : fs::recursive_directory_iterator(cache))
        if (entry.path().extension() == ".shaderpkg")
            result.emplace(entry.path().lexically_relative(cache).generic_string(),
                           entry.last_write_time());
    return result;
}
} // namespace

TEST(MaterialVariantCookIntegration, CommittedGraphSamplesCookAndVerifyWithoutACompiler)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_sample_cook")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(project.Path()));
    const auto result = Cook(project.Path());
    ASSERT_NO_FATAL_FAILURE(CheckCook(result));
    // Every particle shape cooks exactly the particle renderer's request set, not the mesh-material table.
    std::set<std::string> requested;
    for (const auto& variant : GameEngine::Particles::ParticleRenderVariants())
        requested.insert(variant.Name);
    const std::size_t shapes = GameEngine::Particles::ParticleRenderMaterialShapes().size();
    for (std::size_t shape = 0; shape < shapes; ++shape)
    {
        std::set<std::string> cooked;
        for (const auto& [variant, package] : CookedPackages(result.output, "particles-" + std::to_string(shape)))
            cooked.insert(variant);
        EXPECT_EQ(cooked, requested) << "particles-" << shape;
    }
    const fs::path generated = project.Path() / ".Cache/Shaders/Generated";
    ASSERT_TRUE(fs::is_directory(generated)) << generated;
    std::size_t materializedSamples = 0;
    for (const auto& entry : fs::recursive_directory_iterator(generated))
        if (entry.path().extension() == ".glsl")
            ++materializedSamples;
    EXPECT_EQ(materializedSamples, 2u);
}

// The export reads the cook's report line by line, so compiling variants concurrently must report
// them, and name their packages, exactly as compiling them one at a time does.
TEST(MaterialVariantCookIntegration, ConcurrentCookReportsEveryVariantInTheSerialOrder)
{
    GameEngine::TestUtils::ScopedTempDir serial{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_cook_serial")};
    GameEngine::TestUtils::ScopedTempDir concurrent{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_cook_concurrent")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(serial.Path()));
    ASSERT_NO_FATAL_FAILURE(WriteLobeMaterials(serial.Path()));
    fs::copy(serial.Path(), concurrent.Path(), fs::copy_options::recursive);

    const auto serialCook = Cook(serial.Path(), false, {"--particle-row", "base", "--jobs", "1"});
    ASSERT_NO_FATAL_FAILURE(CheckCook(serialCook));
    const auto concurrentCook = Cook(concurrent.Path(), false, {"--particle-row", "base", "--jobs", "8"});
    ASSERT_NO_FATAL_FAILURE(CheckCook(concurrentCook));

    // Each cook names packages under its own cache; the key directories must match.
    std::vector<std::string> expected = ReportLines(serialCook.output);
    ASSERT_GT(expected.size(), 1u) << serialCook.output;
    const std::string serialCache = (serial.Path() / ".Cache" / "Shaders").make_preferred().string();
    const std::string concurrentCache = (concurrent.Path() / ".Cache" / "Shaders").make_preferred().string();
    for (std::string& line : expected)
        if (const std::size_t at = line.find(serialCache); at != std::string::npos)
            line.replace(at, serialCache.size(), concurrentCache);
    EXPECT_EQ(ReportLines(concurrentCook.output), expected);
}

// A program the editor already compiled is the same program under the same key: a cook given the
// editor's cache copies every program it holds into its own cache and compiles none of them.
TEST(MaterialVariantCookIntegration, ACookReusesWhatTheEditorCacheHolds)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_cook_reuse")};
    ASSERT_NO_FATAL_FAILURE(WriteLobeMaterials(project.Path()));
    const fs::path editorCache = project.Path() / ".Cache/Shaders";
    const auto seeded = Cook(project.Path(), false, {"--particle-row", "base"});
    ASSERT_FALSE(seeded.cancelled) << seeded.output;
    ASSERT_EQ(seeded.exitCode, 0) << seeded.output;
    const auto before = PackageWriteTimes(editorCache);
    ASSERT_FALSE(before.empty());

    const fs::path exportCache = project.Path() / ".Build/MaterialVariantCook";
    const auto result = Cook(project.Path(), false,
                             {"--particle-row", "base", "--cache", exportCache.string(),
                              "--reuse-cache", editorCache.string()});
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;
    const std::vector<fs::path> packages = GameEngine::CookedShaderPackages(result.output);
    ASSERT_FALSE(packages.empty()) << result.output;
    for (const fs::path& package : packages)
        EXPECT_TRUE(fs::is_regular_file(package) && !package.lexically_relative(exportCache).empty() &&
                    *package.lexically_relative(exportCache).begin() != "..")
            << package << " is not a program in the export's cache";
    const std::size_t variants = ReportLines(result.output).size();
    EXPECT_NE(result.output.find("MaterialVariantCook: " + std::to_string(variants) + " variant(s) reused from"),
              std::string::npos)
        << "every variant was in the editor's cache, so none may compile\n" << result.output;
    EXPECT_EQ(PackageWriteTimes(editorCache), before) << "the cook wrote to the editor's cache";
}

// The export reads the result lines and the summary from this output. The verify pass logs a line
// per program just before the summary: every logged line is written before the summary, so no log
// line follows it. A desktop cook without --reuse-cache prints a single summary line, which is
// therefore the last line of its output.
TEST(MaterialVariantCookIntegration, NoLogLineFollowsTheCookSummary)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_cook_summary")};
    ASSERT_NO_FATAL_FAILURE(WriteLobeMaterials(project.Path()));
    const auto result = Cook(project.Path(), false, {"--particle-row", "base"});
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;
    const std::string& output = result.output;
    const std::string tail = output.substr(output.size() - std::min<std::size_t>(output.size(), 4096));
    ASSERT_TRUE(output.ends_with('\n')) << "the output ends mid-line:\n" << tail;
    const std::size_t lastLineEnd = output.find_last_not_of("\r\n") + 1;
    const std::size_t lastLineStart = output.find_last_of("\r\n", lastLineEnd - 1) + 1;
    const std::string lastLine = output.substr(lastLineStart, lastLineEnd - lastLineStart);
    EXPECT_TRUE(lastLine.starts_with("MaterialVariantCook: ") &&
                lastLine.find(" variant(s) cooked from ") != std::string::npos)
        << "the last line is not the summary:\n" << tail;
}

TEST(MaterialVariantCookIntegration, RelocatedGraphSamplesReuseEveryCookedPackage)
{
    GameEngine::TestUtils::ScopedTempDir original{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_original")};
    GameEngine::TestUtils::ScopedTempDir relocated{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_relocated")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(original.Path()));
    ASSERT_NO_FATAL_FAILURE(CheckCook(Cook(original.Path())));
    fs::copy(original.Path(), relocated.Path(), fs::copy_options::recursive);
    const fs::path cache = relocated.Path() / ".Cache/Shaders";
    const auto before = PackageWriteTimes(cache);
    ASSERT_FALSE(before.empty());
    ASSERT_NO_FATAL_FAILURE(CheckCook(Cook(relocated.Path())));
    EXPECT_EQ(PackageWriteTimes(cache), before)
        << "Relocating a cooked project must neither add nor rewrite shader packages";
}

// A cooked graph variant is found at runtime only if the cook and the runtime
// name its generated surface alike: the cook from its --project and --scan
// roots, the runtime from its mount table. The samples sit at one relative path
// in the project, in an embedded package whose alias the cook derives from its
// manifest, and in a root named by a mixed-case --scan-alias that the runtime
// lower-cases; no two of them may share a surface.
TEST(MaterialVariantCookIntegration, CookNamesGraphSurfacesLikeTheRuntimeMountTable)
{
    using namespace GameEngine;
    TestUtils::ScopedTempDir project{TestUtils::MakeUniqueTempDirectory("material_graph_mounts")};
    TestUtils::ScopedTempDir extra{TestUtils::MakeUniqueTempDirectory("material_graph_extra")};
    TestUtils::ScopedTempDir runtime{TestUtils::MakeUniqueTempDirectory("material_graph_runtime")};
    const fs::path package = project.Path() / "Packages/graph.samples_kit";
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(project.Path()));
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(package));
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(extra.Path()));
    std::ofstream(package / "package.json") << R"({"name": "graph.samples_kit", "version": "1.0.0"})";
    std::ofstream(project.Path() / "Packages/manifest.json")
        << R"({"dependencies": {"graph.samples_kit": "embedded"}})";

    const ShellProcessResult cooked =
        Cook(project.Path(), false,
             {"--scan", (package / "Assets").string(), "--scan", (extra.Path() / "Assets").string(),
              "--scan-alias", "Graph-Samples"});
    ASSERT_FALSE(cooked.cancelled) << cooked.output;
    ASSERT_EQ(cooked.exitCode, 0) << cooked.output;

    AssetManager assets;
    ASSERT_TRUE(assets.Initialize(project.Path() / "Assets", nullptr,
                                  runtime.Path() / "registry.assetdb", runtime.Path() / "Cache"));
    const PackageResolution packages =
        PackageResolver::Resolve(project.Path(), runtime.Path() / "NoEnginePackages");
    ASSERT_TRUE(packages.Errors.empty()) << packages.Errors.front();
    ASSERT_EQ(MountResolvedPackages(assets, packages, runtime.Path() / "AssetDatabase").size(), 1u);
    AssetSourceDesc extraSource{};
    extraSource.Alias = "Graph-Samples";
    extraSource.Root = extra.Path() / "Assets";
    ASSERT_TRUE(assets.RegisterSource(extraSource));
    Rendering::MaterialBuildContext context;
    context.ProjectRoots = CollectProjectRoots(assets);
    context.AssetSourceRoots = CollectAssetSourceRoots(assets);
    context.CacheRoot = runtime.Path() / "Shaders";
    assets.Shutdown();

    const std::set<std::string> cookedNames = FileNames(project.Path() / ".Cache/Shaders/Generated");
    std::set<std::string> runtimeNames;
    for (const fs::path& root : {project.Path(), package, extra.Path()})
    {
        for (const std::string sample : {"GraphSamplePBR", "PreviewSphereTest"})
        {
            const fs::path material = root / "Assets/Materials/Graph" / (sample + ".material");
            MaterialAsset asset(GUID::Generate(), material);
            ASSERT_TRUE(asset.Load()) << material;
            const Engine::Renderer::MaterialShaderPackageBuilder builder(
                asset.GetDocument(), material, sample, context);
            ASSERT_TRUE(builder.IsPrepared())
                << material << ": "
                << (builder.GetErrors().empty() ? std::string{} : builder.GetErrors().front());
            const std::string name =
                fs::path(builder.GetDocument().surfaceShader).filename().string();
            EXPECT_TRUE(cookedNames.contains(name))
                << material << ": the runtime asks for " << name << ", which the cook did not write";
            runtimeNames.insert(name);
        }
    }
    EXPECT_EQ(runtimeNames.size(), 6u);
    EXPECT_EQ(cookedNames, runtimeNames);
}

// The cook refuses a scan alias the runtime would refuse to mount, and a scan
// root whose alias it cannot derive, before cooking anything.
TEST(MaterialVariantCookIntegration, CookRefusesScanAliasesTheRuntimeCannotMount)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_scan_alias_refusals")};
    const fs::path scan = project.Path() / "Loose/Assets";
    fs::create_directories(project.Path() / "Assets");
    fs::create_directories(scan);
    const struct
    {
        std::vector<std::string> Arguments;
        const char* Expected;
    } cases[] = {
        {{"--scan", scan.string(), "--scan-alias", ""}, "is not a valid source alias"},
        {{"--scan", scan.string(), "--scan-alias", "@scope/My Pkg"}, "is not a valid source alias"},
        {{"--scan", scan.string()}, "pass --scan-alias"},
        {{"--particle-row", "nope"}, "--particle-row nope: not a row of the particle request set"},
        // strtoul would take these: "-1" wrapping to unbounded workers, the sign, the space.
        {{"--jobs", "-1"}, "--jobs needs a positive whole number"},
        {{"--jobs", "+4"}, "--jobs needs a positive whole number"},
        {{"--jobs", " 3"}, "--jobs needs a positive whole number"},
        {{"--jobs", "0"}, "--jobs needs a positive whole number"},
    };
    for (const auto& refusal : cases)
    {
        const GameEngine::ShellProcessResult result = Cook(project.Path(), false, refusal.Arguments);
        EXPECT_EQ(result.exitCode, 2) << result.output;
        EXPECT_NE(result.output.find(refusal.Expected), std::string::npos) << result.output;
    }
}

// The desktop cook composes for a desktop device with every feature, as that device's runtime does:
// every variant it writes for an opaque relief material shades, marches and takes the march's
// footprint at the pixel centre, so each declares the interpolation functions. A cook composing
// without them writes programs under keys that runtime never asks for, and the runtime then compiles
// every parallax program at first draw.
TEST(MaterialVariantCookIntegration, TheDesktopCookComposesTheReliefForADeviceWithInterpolationFunctions)
{
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_relief_cook")};
    ASSERT_NO_FATAL_FAILURE(WriteReliefMaterial(project.Path()));
    const GameEngine::ShellProcessResult result = Cook(project.Path());
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;

    const auto packages = CookedPackages(result.output, "Relief");
    ASSERT_FALSE(packages.empty()) << "the relief material was not cooked\n" << result.output;
    for (const auto& [variant, path] : packages)
    {
        GameEngine::Rendering::ShaderPackage package{};
        std::string error;
        ASSERT_TRUE(GameEngine::Rendering::LoadShaderPkg(path.string(), GameEngine::Rendering::ShaderSourceKind::SpirV,
                                                         package, &error))
            << variant << ": " << error;
        const auto fragment = package.stageBytes.find("fs");
        ASSERT_NE(fragment, package.stageBytes.end()) << variant << " has no fragment stage";
        EXPECT_TRUE(DeclaresCapability(fragment->second, kCapabilityInterpolationFunction))
            << variant << " marches without the interpolation functions a desktop device composes it with";
    }
}

// A desktop export cooks with DesktopMaterialCookArguments into its own cache and ships the packages
// the cook names in the bundle's read-only shader cache (StageCookedShaderPackages). A packaged game then
// serves every program it asks for from that bundle, without a compiler and without writing to its own
// cache. The keys are every one the runtime's prewarm asks for (LOD-crossfade tails and depth heads
// included), under the shipped graphs' world passes and every per-view one (ColourWorldPasses); each
// carries the material's lobes (clear coat, transmission and the rest DeriveBaseVariantKey reads from
// the document), and a transmissive material's include its scene-colour-grab variants for the
// transmissive pass. The imported mesh materials' keys are checked too, each colour-pass key also on
// its vertex-colour layout, as a mesh with a COLOR_0 stream draws it. Each key is resolved here as the
// runtime's ShaderCompilationCache builds it (ExpectServedFromBundle).
TEST(MaterialVariantCookIntegration, AnExportedBundleServesEveryKeyTheRuntimePrewarms)
{
    using namespace GameEngine;
    TestUtils::ScopedTempDir project{TestUtils::MakeUniqueTempDirectory("material_lobe_cook")};
    ASSERT_NO_FATAL_FAILURE(WriteLobeMaterials(project.Path()));
    const fs::path fixtures = TestPaths::StagedRoot();
    DesktopMaterialCookInputs inputs;
    inputs.ProjectAssets = project.Path() / "Assets";
    inputs.EngineShaders = fixtures / "Engine/Modules/Rendering/Shaders";
    inputs.PackageShaderDirs = {fixtures / "Engine/Modules/GPUFogParticles/Shaders",
                                TestPaths::ExecutableDirectory() / "Fixtures/MaterialVariantCook/PackageShaders"};
    inputs.Cache = project.Path() / ".Build/MaterialVariantCook";
    const fs::path tool = TestPaths::ExecutableDirectory().parent_path() / "Tools" / GE_MATERIAL_VARIANT_COOK_FILENAME;
    const ShellProcessResult result =
        RunProcessCaptured(tool.string(), DesktopMaterialCookArguments(inputs), std::chrono::minutes(45));
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;

    const std::vector<fs::path> packages = CookedShaderPackages(result.output);
    ASSERT_FALSE(packages.empty()) << result.output;
    const fs::path bundle = project.Path() / "Game.app/Contents/Resources/.Cache/Shaders";
    std::string stageError;
    ASSERT_TRUE(StageCookedShaderPackages(packages, inputs.Cache, bundle, stageError)) << stageError;
    for (const fs::path& package : packages)
        EXPECT_TRUE(fs::is_regular_file(bundle / package.lexically_relative(inputs.Cache))) << package;

    Rendering::MaterialBuildContext context;
    context.AdapterShaderDir = inputs.EngineShaders;
    context.PackageShaderDirs = inputs.PackageShaderDirs;
    context.ProjectRoots = {inputs.ProjectAssets};
    context.AssetSourceRoots.push_back({"project", inputs.ProjectAssets});
    context.ReadOnlyCacheRoots = {bundle};
    const fs::path userCache = project.Path() / "UserData/.Cache/Shaders";
    context.CacheRoot = userCache;
    ScopedDesktopCompileInputs desktopInputs;

    std::size_t grabKeys = 0;
    std::size_t vertexColourKeys = 0;
    {
        ScopedCookOnlyRuntime cookOnly;
        for (const char* name : {"ClearCoat", "Glass"})
        {
            const fs::path materialPath = project.Path() / "Assets/Materials" / (std::string(name) + ".material");
            MaterialAsset asset(GUID::Generate(), materialPath);
            ASSERT_TRUE(asset.Load()) << materialPath;
            const auto keys = RuntimePrewarmKeys(asset.GetDocument(), materialPath);
            ASSERT_FALSE(keys.empty()) << name;
            for (const auto& key : keys)
                grabKeys += Rendering::HasKeyword(key.materialKeywords, Rendering::MaterialKeyword::SceneColorGrab);
            ExpectServedFromBundle(asset.GetDocument(), materialPath, name, keys, context, bundle);
        }
        // An imported model's materials compose as the default PBR family, and a mesh with a COLOR_0 stream
        // draws every colour-pass key of theirs, crossfade tails included, with the stream bound
        // (BoundStreamVertexFlags). The blended stand-in cooks only the rows its own table declares.
        for (const auto& imported : Tools::MaterialCook::kImportedMeshMaterialTargets)
        {
            if (!imported.Rows.empty())
                continue;
            MaterialDocument document = MaterialDocument::CreateDefaultPBR(imported.DocumentName);
            document.alphaMode = imported.AlphaMode;
            const fs::path materialPath = project.Path() / "Assets/Models" / imported.File;
            auto keys = RuntimePrewarmKeys(document, materialPath);
            for (std::size_t i = 0, count = keys.size(); i < count; ++i)
            {
                if (!Rendering::HasKeyword(keys[i].materialKeywords, Rendering::MaterialKeyword::ForwardPlus))
                    continue;
                auto coloured = keys[i];
                coloured.vertexFlags |= Rendering::VertexAttributeFlags::HasColor;
                if (std::find(keys.begin(), keys.end(), coloured) != keys.end())
                    continue;
                keys.push_back(coloured);
                ++vertexColourKeys;
            }
            ExpectServedFromBundle(document, materialPath, imported.Name, keys, context, bundle);
        }
    }
    EXPECT_GT(grabKeys, 0u) << "the glass asks for no scene-colour grab variant, so the test checks none";
    EXPECT_GT(vertexColourKeys, 0u) << "no imported material asks for a vertex-colour key, so the test checks none";
    std::size_t userPackages = 0;
    if (fs::is_directory(userCache))
        for (const auto& entry : fs::recursive_directory_iterator(userCache))
            userPackages += entry.path().extension() == ".shaderpkg";
    EXPECT_EQ(userPackages, 0u) << "a program served from the bundle was written to the user cache";
}

// A desktop runtime compiles the terrain grass placement's compute programs from the engine shader tree,
// and registers the grass draw-mode materials when it first draws grass. An export's cook compiles both
// into the bundle, so the packaged game serves the compute programs, built as the runtime builds them
// (MakeGrassComputeCompileRequest), from the bundle without a compiler.
TEST(MaterialVariantCookIntegration, AnExportedBundleServesTheTerrainGrassPrograms)
{
    using namespace GameEngine;
    TestUtils::ScopedTempDir project{TestUtils::MakeUniqueTempDirectory("material_grass_cook")};
    // The engine shader tree an export ships: the engine's shaders with the modules' staged beside them.
    const fs::path fixtures = TestPaths::StagedRoot();
    const fs::path packageShaders = TestPaths::ExecutableDirectory() / "Fixtures/MaterialVariantCook/PackageShaders";
    const fs::path engineShaders = project.Path() / "Content/Assets/Shaders";
    fs::create_directories(engineShaders);
    fs::copy(fixtures / "Engine/Modules/Rendering/Shaders", engineShaders, fs::copy_options::recursive);
    fs::copy(packageShaders, engineShaders, fs::copy_options::recursive | fs::copy_options::skip_existing);
    fs::create_directories(project.Path() / "Assets");

    DesktopMaterialCookInputs inputs;
    inputs.ProjectAssets = project.Path() / "Assets";
    inputs.EngineShaders = engineShaders;
    inputs.PackageShaderDirs = {fixtures / "Engine/Modules/GPUFogParticles/Shaders", packageShaders};
    inputs.Cache = project.Path() / ".Build/MaterialVariantCook";
    std::vector<std::string> arguments = DesktopMaterialCookArguments(inputs);
    arguments.insert(arguments.end(), {"--particle-row", "base"});
    const fs::path tool = TestPaths::ExecutableDirectory().parent_path() / "Tools" / GE_MATERIAL_VARIANT_COOK_FILENAME;
    const ShellProcessResult result = RunProcessCaptured(tool.string(), arguments, std::chrono::minutes(45));
    ASSERT_FALSE(result.cancelled) << result.output;
    ASSERT_EQ(result.exitCode, 0) << result.output;
    EXPECT_GT(CookedVariantCount(result.output, "terrain-grass-opaque"), 0u) << result.output;
    EXPECT_EQ(CookedVariantCount(result.output, "terrain-grass-compute"), std::size(TerrainGrass::kGrassComputePrograms))
        << result.output;

    const fs::path bundle = project.Path() / "Game.app/Contents/Resources/.Cache/Shaders";
    std::string stageError;
    ASSERT_TRUE(StageCookedShaderPackages(CookedShaderPackages(result.output), inputs.Cache, bundle, stageError))
        << stageError;

    ScopedDesktopCompileInputs desktopInputs;
    ScopedCookOnlyRuntime cookOnly;
    const fs::path userCache = project.Path() / "UserData/.Cache/Shaders";
    for (const TerrainGrass::GrassComputeProgram& program : TerrainGrass::kGrassComputePrograms)
    {
        Rendering::ShaderProgramCompileRequest request =
            TerrainGrass::MakeGrassComputeCompileRequest(engineShaders, program, false);
        request.readOnlyCacheRoots = {bundle};
        request.cacheRoot = userCache;
        Rendering::ShaderProgramCompileResult served{};
        std::string error;
        EXPECT_TRUE(Rendering::ShaderCompileService::CompileProgramToCache(request, Rendering::ShaderSourceKind::SpirV,
                                                                         served, &error))
            << program.ShaderFile << " was not cooked: " << error;
        EXPECT_EQ(served.shaderPkgPath.parent_path().parent_path(), bundle)
            << program.ShaderFile << " was served from " << served.shaderPkgPath << ", not from the bundle";
    }
}

TEST(MaterialVariantCookIntegration, CommittedGraphSamplesCookValidatedWebPackages)
{
    const char* configuredPython = std::getenv("GE_PYTHON");
    const auto preflight = GameEngine::RunProcessCaptured(
        configuredPython ? configuredPython : "python3",
        {ShaderCookScript().string(), "--self-test"});
    if (preflight.exitCode != 0)
        GTEST_SKIP() << "WGSL toolchain unavailable (glslc/naga/tint): " << preflight.output;
    GameEngine::TestUtils::ScopedTempDir project{
        GameEngine::TestUtils::MakeUniqueTempDirectory("material_graph_web_cook")};
    ASSERT_NO_FATAL_FAILURE(CopyGraphSamples(project.Path()));
    // Each particle shape cooks one row, the keyword-less base program; it composes both
    // stages' particle varyings, which WGSL must accept as stage interfaces. The export
    // cooks the whole request set (Particles::ParticleRenderVariants).
    const auto result = Cook(project.Path(), true, {"--particle-row", "base"});
    ASSERT_NO_FATAL_FAILURE(CheckCook(result));
    const std::size_t shapes = GameEngine::Particles::ParticleRenderMaterialShapes().size();
    ASSERT_GT(shapes, 0u);
    for (std::size_t shape = 0; shape <= shapes; ++shape)
        EXPECT_EQ(CookedVariantCount(result.output, "particles-" + std::to_string(shape)),
                  shape < shapes ? 1u : 0u)
            << "particles-" << shape << " of " << shapes << " shapes\n" << result.output;
}
