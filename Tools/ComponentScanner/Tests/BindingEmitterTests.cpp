// The binding generators end to end (--emit-native, --emit-csharp): the native and the C# binding of
// a fixture script API with every shape the generators emit, byte for byte against the committed
// files of Fixtures/Bindings/Expected, and the refusal of each declaration they cannot emit.
#include "ComponentScanner.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{

const fs::path kFixture = fs::path(GE_COMPONENT_SCANNER_FIXTURES_DIR) / "Bindings";

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string Utf8Text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

TEST(ScriptBindings, NativeAndCSharpBindingsOfTheProbeAreTheGoldens)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerBindings"));
    const fs::path native = scratch.Path() / "native";
    const fs::path csharp = scratch.Path() / "csharp";
    fs::create_directories(native);
    std::ofstream(native / "Removed.script.gen.cpp") << "";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--api-modules", Utf8Text(kFixture / "Good"), "--emit-api", Utf8Text(scratch.Path() / "api"), "--emit-native",
         Utf8Text(native), "--emit-csharp", Utf8Text(csharp)},
        out, err);

    ASSERT_EQ(0, exitCode) << err.str();
    for (const char* file : {"Probe.script.gen.h", "Probe.script.gen.cpp"})
        EXPECT_EQ(ReadFile(kFixture / "Expected" / file), ReadFile(native / file)) << file;
    EXPECT_EQ(ReadFile(kFixture / "Expected" / "Probe.g.cs"), ReadFile(csharp / "Probe.g.cs"));
    EXPECT_FALSE(fs::exists(native / "Removed.script.gen.cpp")) << "a module that is gone leaves no binding behind";
}

// The slice-1 fixture holds every kind of the description; the generators refuse the ones they do
// not emit, with the header's path as the scanner prints it, the line and the fix, and write nothing.
TEST(ScriptBindings, RefusesWhatTheGeneratorsDoNotEmit)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerBindings"));
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--api-modules", Utf8Text(fs::path(GE_COMPONENT_SCANNER_FIXTURES_DIR) / "ScriptApi" / "Good"), "--emit-api",
         Utf8Text(scratch.Path() / "api"), "--emit-csharp", Utf8Text(scratch.Path() / "csharp")},
        out, err);

    EXPECT_EQ(1, exitCode);
    const std::string text = err.str();
    const std::string header =
        Utf8Text(fs::path(GE_COMPONENT_SCANNER_FIXTURES_DIR) / "ScriptApi" / "Good" / "Kinds" / "Include" / "Kinds" / "KindsScriptApi.h");
    EXPECT_EQ(text.find("the String kind"), std::string::npos) << "the generators emit text";
    EXPECT_EQ(text.find("the ColorLinear kind"), std::string::npos) << "the generators emit a linear color";
    EXPECT_NE(text.find("error: " + header + ":85: the binding generators do not emit the Span kind (parameter `weights`). Fix: "
                        "use a kind of the crossing the generators emit (docs/scripting/exposing-a-function.mdx), or leave the "
                        "declaration unmarked"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("error: " + header + ":122: the binding generators emit a span of results of records or entities only "
                        "(parameter `weights` holds Float). Fix: write the results as a marked record, or as entities"),
              std::string::npos)
        << text;
    EXPECT_EQ(text.find("parameter `found`"), std::string::npos) << "the generators emit a span of records";
    EXPECT_NE(text.find("error: " + header + ":131: the binding generators emit a RenderLayerMask as a parameter or a return value "
                        "only (field `layers` of `Listener`). Fix: hold the mask's bits as a std::uint32_t field"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("the binding generators emit events with no @ge-context (`ContactBegin`)"), std::string::npos) << text;
    EXPECT_FALSE(fs::exists(scratch.Path() / "api")) << "a refused scan writes nothing";
    EXPECT_FALSE(fs::exists(scratch.Path() / "csharp"));
}
