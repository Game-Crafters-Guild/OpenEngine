// The user modes end to end: --detect-base registers inheritors with include paths relative
// to the scanned root and warns about a component stranded in a .cpp;
// --detect-system-base emits the system registration TU. The expected files are the
// generator's committed output for Fixtures/UserScan.
#include "ComponentScanner.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace GameEngine::ComponentScanner;
namespace fs = std::filesystem;

namespace
{

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const fs::path kFixture = fs::path(GE_COMPONENT_SCANNER_FIXTURES_DIR) / "UserScan";

// The path as the UTF-8 text Run takes.
std::string Utf8Text(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

TEST(ScanMode, DetectBaseRegistersInheritorsRelativeToTheScannedRoot)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerUser"));
    const fs::path outPath = scratch.Path() / "UserComponentReflection.gen.cpp";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run({"--in", (kFixture / "Components").string(), "--out", outPath.string(), "--detect-base", "ComponentBase"}, out, err);

    EXPECT_EQ(0, exitCode) << err.str();
    EXPECT_EQ(ReadFile(kFixture / "Expected" / "UserComponentReflection.gen.cpp"), ReadFile(outPath));
    EXPECT_NE(std::string::npos, out.str().find("ComponentScanner.summary reflected=2 skipped=0 sourcewarnings=1"));
    EXPECT_NE(std::string::npos, err.str().find("warning: 'Stray' inherits ComponentBase in"));
}

TEST(ScanMode, DetectSystemBaseEmitsTheSystemRegistrationUnit)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerSystem"));
    const fs::path outPath = scratch.Path() / "UserSystemRegistration.gen.cpp";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run({"--in", (kFixture / "Components").string(), "--out", outPath.string(), "--detect-system-base", "SystemBase"}, out, err);

    EXPECT_EQ(0, exitCode) << err.str();
    EXPECT_EQ(ReadFile(kFixture / "Expected" / "UserSystemRegistration.gen.cpp"), ReadFile(outPath));
    EXPECT_NE(std::string::npos, out.str().find("ComponentScanner.summary systems=2"));
}

TEST(ScanMode, BadUsageIsReportedWithExitCodeTwo)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(2, GameEngine::ComponentScanner::Run({"--bogus"}, out, err));
    EXPECT_NE(std::string::npos, err.str().find("Unknown argument: --bogus"));

    err.str("");
    EXPECT_EQ(2, GameEngine::ComponentScanner::Run({"--in"}, out, err));
    EXPECT_NE(std::string::npos, err.str().find("Missing value for --in"));

    err.str("");
    EXPECT_EQ(2, GameEngine::ComponentScanner::Run({"--in", (kFixture / "Components").string(), "--emit-dts", "x.d.ts", "--detect-system-base", "SystemBase"}, out, err));
    EXPECT_NE(std::string::npos, err.str().find("do not apply with --detect-system-base"));

    err.str("");
    EXPECT_EQ(2, GameEngine::ComponentScanner::Run({"--in", (kFixture / "DoesNotExist").string()}, out, err));
    EXPECT_NE(std::string::npos, err.str().find("Input directory does not exist"));
}

// The arguments are UTF-8 on every platform: a project under a folder the Windows ANSI code
// page cannot spell still scans.
TEST(ScanMode, ScansARootWhosePathIsNotAscii)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerUtf8"));
    const fs::path root = scratch.Path() / fs::path(u8"プロジェクト") / "Components";
    fs::create_directories(root);
    std::ofstream(root / "Speed.h", std::ios::binary) << "struct Speed : ECS::ComponentBase { float Value; };\n";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--in", Utf8Text(root), "--out", Utf8Text(root.parent_path() / "Out.gen.cpp"), "--detect-base", "ComponentBase"}, out, err);

    EXPECT_EQ(0, exitCode) << err.str();
    EXPECT_NE(std::string::npos, out.str().find("ComponentScanner.summary reflected=1 skipped=0"));
    EXPECT_TRUE(fs::exists(root.parent_path() / "Out.gen.cpp"));
}

// A game's own component with an AssetRef<...> field reflects; any other templated field still
// skips its struct with the reason.
TEST(ScanMode, DetectBaseReflectsAnAssetRefFieldAndSkipsOtherTemplates)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerAssetRef"));
    const fs::path root = scratch.Path() / "Components";
    fs::create_directories(root);
    std::ofstream(root / "Pickup.h", std::ios::binary)
        << "struct Pickup : ECS::ComponentBase { GameEngine::Components::AssetRef<AssetType::Audio> Sound; float Radius = 1.0f; };\n"
           "struct Stack : ECS::ComponentBase { Handle<int> Top; };\n";
    const fs::path outPath = scratch.Path() / "Out.gen.cpp";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--in", Utf8Text(root), "--out", Utf8Text(outPath), "--detect-base", "ComponentBase"}, out, err);

    EXPECT_EQ(0, exitCode) << err.str();
    EXPECT_NE(std::string::npos, out.str().find("ComponentScanner.summary reflected=1 skipped=1")) << out.str();
    EXPECT_NE(std::string::npos, ReadFile(outPath).find("GE_REGISTER_COMPONENT_FIELD(Pickup, Sound)"));
    EXPECT_NE(std::string::npos, out.str().find("Stack [Pickup.h] -- field 'Top' type 'Handle<int>' is templated")) << out.str();
}

// A game's component naming an enum the game declares in a header outside the scanned folder, under
// an include root it passes, gets the enum's name table the same way an engine component does.
TEST(ScanMode, DetectBaseResolvesAnEnumFromAnIncludedHeaderUnderAnIncludeRoot)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerEnumRoot"));
    const fs::path root = scratch.Path() / "Components";
    const fs::path includeRoot = scratch.Path() / "Include";
    fs::create_directories(root);
    fs::create_directories(includeRoot / "Shared");
    std::ofstream(includeRoot / "Shared" / "Faction.h", std::ios::binary)
        << "#pragma once\nnamespace Game { enum class Faction : uint8 { Neutral = 0, Raiders = 2 }; }\n";
    std::ofstream(root / "Allegiance.h", std::ios::binary)
        << "#include \"Shared/Faction.h\"\nstruct Allegiance : ECS::ComponentBase { Game::Faction Side = Game::Faction::Neutral; };\n";
    const fs::path outPath = scratch.Path() / "Out.gen.cpp";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run({"--in", Utf8Text(root), "--enum-include-root", Utf8Text(includeRoot),
                                                            "--out", Utf8Text(outPath), "--detect-base", "ComponentBase"},
                                                           out, err);

    EXPECT_EQ(0, exitCode) << err.str();
    const std::string generated = ReadFile(outPath);
    EXPECT_NE(std::string::npos, generated.find("GE_REFLECT_ENUM_TABLE_VALUE(\"Raiders\", 2)")) << generated;
    EXPECT_NE(std::string::npos, generated.find("GE_REFLECT_ENUM_FIELD(Allegiance, Side, Faction)")) << generated;
}
