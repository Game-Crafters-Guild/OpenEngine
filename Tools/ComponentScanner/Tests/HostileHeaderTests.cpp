// Headers the scanner reads without a preprocessor or a full lexer: each one is scanned end to
// end in user mode, and the scan reflects what the compiler would see, or names what it skipped
// with the file and line. A construct the scanner cannot read never drops the rest of the header
// without a word, and never crashes it.
#include "ComponentScanner.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

struct ScanResult
{
    int ExitCode = -1;
    std::string Out;
    std::string Err;
};

// Writes `header` as Components/A.h under a fresh directory and scans it for ComponentBase
// inheritors.
ScanResult ScanHeader(const std::string& header)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerHostile"));
    const fs::path root = scratch.Path() / "Components";
    fs::create_directories(root);
    std::ofstream(root / "A.h", std::ios::binary) << header;

    std::ostringstream out;
    std::ostringstream err;
    const std::u8string rootText = root.u8string();
    ScanResult result;
    result.ExitCode = GameEngine::ComponentScanner::Run(
        {"--in", std::string(rootText.begin(), rootText.end()), "--dry-run", "--detect-base", "ComponentBase"}, out, err);
    result.Out = out.str();
    result.Err = err.str();
    return result;
}

bool Reflects(const ScanResult& result, const std::string& name)
{
    return result.Out.find("\n  " + name + " (") != std::string::npos;
}

} // namespace

TEST(HostileHeader, ComponentInsideIfZeroIsNotReflected)
{
    const ScanResult result = ScanHeader("#if 0\n"
                                         "struct Disabled : GameEngine::ECS::ComponentBase { float Z; };\n"
                                         "#endif\n"
                                         "struct Good : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_FALSE(Reflects(result, "Disabled")) << result.Out;
    EXPECT_TRUE(Reflects(result, "Good")) << result.Out;
}

TEST(HostileHeader, MacroDeclaredComponentIsNamedInAWarningNotRegisteredByItsParameter)
{
    const ScanResult result =
        ScanHeader("#define GE_DECLARE(N) struct N : GameEngine::ECS::ComponentBase { float V; };\n"
                   "GE_DECLARE(FromMacro)\n"
                   "struct Good : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_FALSE(Reflects(result, "N")) << result.Out;
    EXPECT_TRUE(Reflects(result, "Good")) << result.Out;
    EXPECT_NE(std::string::npos, result.Err.find("A.h:1: macro 'GE_DECLARE' declares a struct inheriting ComponentBase"))
        << result.Err;
    EXPECT_NE(std::string::npos, result.Err.find("GE_REGISTER_COMPONENT")) << result.Err;
}

TEST(HostileHeader, ApostropheInADirectiveKeepsTheComponentsAfterIt)
{
    const ScanResult result = ScanHeader("#if 0\n"
                                         "#error This header doesn't build here\n"
                                         "#endif\n"
                                         "struct First : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n"
                                         "struct Second : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_NE(std::string::npos, result.Out.find("ComponentScanner.summary reflected=2 skipped=0")) << result.Out;
    EXPECT_EQ("", result.Err);
}

// A string in an unread branch is still a string: its "/*" opens no comment that would run past
// the #endif.
TEST(HostileHeader, CommentOpenerInAStringInsideIfZeroKeepsTheComponentsAfterIt)
{
    const ScanResult result = ScanHeader("#if 0\n"
                                         "inline const char* s = \"/*\";\n"
                                         "#endif\n"
                                         "struct First : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n"
                                         "struct Second : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_NE(std::string::npos, result.Out.find("ComponentScanner.summary reflected=2 skipped=0")) << result.Out;
    EXPECT_EQ("", result.Err);
}

TEST(HostileHeader, RawStringWithAnOddQuoteKeepsTheComponentsAfterIt)
{
    const ScanResult result = ScanHeader("inline const char* kText = R\"(// say \"hi)\";\n"
                                         "struct First : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n"
                                         "struct Second : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_NE(std::string::npos, result.Out.find("ComponentScanner.summary reflected=2 skipped=0")) << result.Out;
    EXPECT_EQ("", result.Err);
}

TEST(HostileHeader, LiteralOpenAtItsLineEndIsReportedAndTheScanGoesOn)
{
    const ScanResult result = ScanHeader("struct First : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n"
                                         "inline const char* kText = \"never closed;\n"
                                         "struct Second : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_NE(std::string::npos, result.Out.find("ComponentScanner.summary reflected=2 skipped=0")) << result.Out;
    EXPECT_NE(std::string::npos, result.Err.find("A.h:2: a string or character literal is not closed on its line"))
        << result.Err;
}

// Twenty thousand nested namespaces overflowed the stack of the recursive scope walk.
TEST(HostileHeader, DeeplyNestedNamespacesAreReportedInsteadOfCrashing)
{
    constexpr int kDepth = 20000;
    std::string header;
    for (int i = 0; i < kDepth; ++i)
        header += "namespace N {\n";
    header += "struct Deep : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n";
    for (int i = 0; i < kDepth; ++i)
        header += "}\n";
    header += "struct Good : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n";

    const ScanResult result = ScanHeader(header);

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_TRUE(Reflects(result, "Good")) << result.Out;
    EXPECT_FALSE(Reflects(result, "Deep")) << result.Out;
    EXPECT_NE(std::string::npos, result.Err.find(":257: namespaces nest deeper than 256")) << result.Err;
}

TEST(HostileHeader, ComponentWithANonAsciiNameIsSkippedWithTheReason)
{
    const ScanResult result = ScanHeader("struct Gr\xC3\xBC\xC3\x9F" "e : GameEngine::ECS::ComponentBase { float Wert = 0.0f; };\n"
                                         "struct Good : GameEngine::ECS::ComponentBase { float X = 0.0f; };\n");

    EXPECT_EQ(0, result.ExitCode) << result.Err;
    EXPECT_NE(std::string::npos, result.Out.find("ComponentScanner.summary reflected=1 skipped=1")) << result.Out;
    EXPECT_NE(std::string::npos, result.Out.find("Gr\xC3\xBC\xC3\x9F" "e [A.h] -- its name is not ASCII")) << result.Out;
}
