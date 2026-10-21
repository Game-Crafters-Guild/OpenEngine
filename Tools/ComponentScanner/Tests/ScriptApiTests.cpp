// The script API scan end to end (--emit-api): the per-module description of a fixture tree
// with every kind of the table, and the named refusal of each declaration the vocabulary does
// not allow. The expected files are the scanner's committed output for Fixtures/ScriptApi/Good.
#include "ComponentScanner.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace
{

const fs::path kFixture = fs::path(GE_COMPONENT_SCANNER_FIXTURES_DIR) / "ScriptApi";

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

// Every .json file of a directory, by file name.
std::map<std::string, std::string> JsonFiles(const fs::path& dir)
{
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".json")
            files[Utf8Text(it->path().filename())] = ReadFile(it->path());
    return files;
}

struct Refusal
{
    const char* Case;
    const char* Modules; // the module tree inside the case, "" for the case folder itself
    const char* Search;  // a search root inside the case, or nullptr
    const char* File;    // inside the case folder
    int Line;
    const char* Message;
};

void PrintTo(const Refusal& refusal, std::ostream* os)
{
    *os << refusal.Case;
}

const Refusal kRefusals[] = {
    {"Overload", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 14,
     "`Play` overloads the marked function of line 10. Fix: give each function its own name"},
    {"VectorParameter", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 10,
     "parameter `sounds`: `std::vector<std::int32_t>` is a standard container, which cannot cross. Fix: take "
     "std::span<const T> for a list, or std::string_view for text"},
    {"IfdefGroup", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 11,
     "the marked declaration is inside the `#ifdef _WIN32` group of line 8, and the description is the same on every "
     "platform. Fix: move the declaration out of the group; a platform difference belongs in the function body"},
    {"MarkerAboveBlankLine", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 9,
     "the marker has no declaration directly below it: a blank line ends the run. Fix: put the marker on the line "
     "directly above the declaration, or delete it"},
    {"EnumWithoutUnderlyingType", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 10,
     "the enum class `Mode` has no explicit underlying type. Fix: write one: `enum class Mode : std::uint8_t`"},
    {"RecordUnknownField", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 13,
     "field `offset`: `glm::vec3` is not in the kinds. Fix: use a kind of the table "
     "(docs/contributing/script-api-markers.mdx), or mark the type in this module's <Module>ScriptApi.h: a struct or an "
     "enum class with // @ge-script, a handle alias with // @ge-script-handle"},
    {"MarkerOutsideScriptApi", "", nullptr, "Bad/Include/Bad/Sound.h", 9,
     "a script marker outside a Bad script API header. Fix: move the declaration to Include/Bad/BadScriptApi.h (or "
     "another Include/Bad/*ScriptApi.h); only those headers are read for script markers"},
    {"Template", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 10,
     "a template cannot be bound. Fix: declare one function per type it serves, each with its own name"},
    {"InstanceMethod", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 12,
     "`const` after the parameter list makes this a member function the binding cannot call. Fix: declare a free "
     "function whose first parameter is the object, marked @ge-context"},
    {"Ambiguous", "", nullptr, "Gamma/Include/Gamma/GammaScriptApi.h", 10,
     "parameter `hit`: `Hit` names Alpha.Hit and Beta.Hit. Fix: qualify the name (`GameEngine::Alpha::Script::Hit` or "
     "`GameEngine::Beta::Script::Hit`), or rename one of the types"},
    {"TripleSlash", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 8,
     "a marker on a `///` line is documentation. Fix: write the marker on its own `//` line"},
    {"MisspelledMarker", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 8,
     "`@ge-any-thead` is not a script API marker. Fix: use one of the markers: @ge-script, @ge-script-handle, @ge-script-event, "
     "@ge-script-try, @ge-context, @ge-engine-thread, @ge-any-thread, @ge-script-flags"},
    {"SearchRoot", "Modules", "Search", "Search/Core/Application.h", 9,
     "a script marker outside a module's script API header. Fix: move the declaration to a module's "
     "Include/<Module>/<Module>ScriptApi.h"},
    {"DefaultOutOfRange", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 10,
     "parameter `scale`: the default `1e400` is outside the range of the parameter's kind. Fix: write a value the kind "
     "can hold"},
    {"MatrixInitializer", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 12,
     "the matrix field `matrix` has an initializer the description cannot read. Fix: leave the field without an "
     "initializer (the identity)"},
    {"PointerOut", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 10,
     "parameter `value`: `float*` is an address. Fix: take T& for the value the function writes"},
    {"FlagsOnStruct", "", nullptr, "Bad/Include/Bad/BadScriptApi.h", 10,
     "@ge-script-flags marks an enum class whose values combine with |. Fix: remove it, or put it on a marked enum class"},
    {"DuplicateModule", "", nullptr, "Beta/Include/Beta/SoundScriptApi.h", 1,
     "the script module `Sound` is already declared by Alpha/SoundScriptApi.h. Fix: rename one of the headers: each "
     "<Name>ScriptApi.h is the <Name> script module"},
};

class ScriptApiRefusal : public testing::TestWithParam<Refusal>
{
};

} // namespace

// One file per script module, named by its header: Events/ClockScriptApi.h is the Clock module.
TEST(ScriptApi, DescribesEveryKindOfTheTableOneFilePerModule)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerApi"));
    const fs::path apiDir = scratch.Path() / "engine-api";
    fs::create_directories(apiDir);
    std::ofstream(apiDir / "Removed.json") << "{}";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--api-modules", Utf8Text(kFixture / "Good"), "--emit-api", Utf8Text(apiDir)}, out, err);

    EXPECT_EQ(0, exitCode) << err.str();
    EXPECT_EQ(JsonFiles(kFixture / "Expected"), JsonFiles(apiDir));
    EXPECT_NE(std::string::npos,
              out.str().find("ComponentScanner.api modules=5 functions=11 records=5 enums=1 handles=2 events=1 refused=0"));
}

TEST_P(ScriptApiRefusal, NamesTheLineAndTheFixAndWritesNothing)
{
    const Refusal& refusal = GetParam();
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerApiRefusal"));
    const fs::path apiDir = scratch.Path() / "engine-api";
    const fs::path tree = kFixture / "Refusals" / refusal.Case;
    std::vector<std::string> args{"--api-modules", Utf8Text(tree / refusal.Modules), "--emit-api", Utf8Text(apiDir)};
    if (refusal.Search != nullptr)
        args.insert(args.end(), {"--api-search", Utf8Text(tree / refusal.Search)});
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(args, out, err);

    const std::string expected =
        "error: " + Utf8Text(tree / refusal.File) + ":" + std::to_string(refusal.Line) + ": " + refusal.Message + "\n";
    EXPECT_EQ(1, exitCode);
    EXPECT_EQ(expected + "The script API description cannot be generated; nothing was written.\n", err.str());
    EXPECT_FALSE(fs::exists(apiDir));
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ScriptApiRefusal, testing::ValuesIn(kRefusals),
                         [](const testing::TestParamInfo<Refusal>& info) { return std::string(info.param.Case); });

TEST(ScriptApi, RunsAsItsOwnInvocation)
{
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(2, GameEngine::ComponentScanner::Run({"--emit-api", "engine-api"}, out, err));
    EXPECT_EQ(2, GameEngine::ComponentScanner::Run({"--api-modules", Utf8Text(kFixture / "Good"), "--emit-api", "engine-api",
                                                    "--in", Utf8Text(kFixture)},
                                                   out, err));
    EXPECT_NE(std::string::npos, err.str().find("--emit-api <dir> takes --api-modules <dir>"));
}
