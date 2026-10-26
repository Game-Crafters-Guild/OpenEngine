// The component mirrors end to end (--api-components): the C# mirrors, the layout pins and the
// classes of a fixture tree with every field shape and every class, byte for byte against the
// committed files of Fixtures/Components/Expected, and the named refusal of each shape the mirror
// rules refuse.
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

const fs::path kFixture = fs::path(GE_COMPONENT_SCANNER_FIXTURES_DIR) / "Components";

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

// Every .g.cs file under a directory, by its path relative to it.
std::map<std::string, std::string> CSharpFiles(const fs::path& dir)
{
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".cs")
            files[Utf8Text(it->path().lexically_relative(dir))] = ReadFile(it->path());
    return files;
}

struct Refusal
{
    const char* Case;
    const char* File; // inside the case folder
    int Line;
    const char* Message; // "{case}" stands for the case folder's path
};

void PrintTo(const Refusal& refusal, std::ostream* os)
{
    *os << refusal.Case;
}

const Refusal kRefusals[] = {
    {"UnknownKind", "Components/Bad.h", 17,
     "the field `Data` of `FixtureBad` has the type `Opaque::Handle`, which no mirror kind covers, so the component cannot be "
     "mirrored. Fix: give it a type a mirror holds (a scalar, a Mathematics type, ColorLinear, ECS::EntityHandle, StringId, "
     "Rendering::RenderLayerMask, an AssetRef, GUID, Math::Curve, Mathematics::BoundingBox, an enum, a reflected struct, or a "
     "fixed array of one of them; a char array for text), or mark the component // @ge-engine-internal when it is engine "
     "runtime state"},
    {"ArrayOfEnums", "Components/Bad.h", 14,
     "the array field `Modes` of `FixtureBad` holds enums, and no setter can check an element written through the array. Fix: "
     "hold the values in an array of the enum's underlying type, or give each element its own field"},
    {"ArrayExtent", "Components/Bad.h", 10,
     "the extent `kSampleCount * 2` of the array field `Samples` of `FixtureBad` is not an integer literal or a sum of constexpr "
     "integer constants. Fix: use a constant in this header"},
    {"ExtentSumOverflow", "Components/Bad.h", 10,
     "the extent `kFixtureHalf + kFixtureHalf` of the array field `Bytes` of `FixtureBad` makes the field larger than "
     "2147483647 bytes, the most a C# struct holds. Fix: hold fewer elements"},
    {"ExtentPastTheCap", "Components/Bad.h", 7,
     "the extent `1073741824` of the array field `Samples` of `FixtureBad` makes the field larger than 2147483647 bytes, the "
     "most a C# struct holds. Fix: hold fewer elements"},
    {"KindNamedOutOfScope", "Components/Bad.h", 16,
     "the field `Shape` of `FixtureBad` has the type `Curve`, which no mirror kind covers, so the component cannot be "
     "mirrored. Fix: give it a type a mirror holds (a scalar, a Mathematics type, ColorLinear, ECS::EntityHandle, StringId, "
     "Rendering::RenderLayerMask, an AssetRef, GUID, Math::Curve, Mathematics::BoundingBox, an enum, a reflected struct, or a "
     "fixed array of one of them; a char array for text), or mark the component // @ge-engine-internal when it is engine "
     "runtime state"},
    {"ArrayNameCollision", "Components/Bad.h", 8,
     "the array field `Weights` of `FixtureBad` needs the nested type `WeightsArray`, which another member of `FixtureBad` "
     "names. Fix: rename the field or the other member"},
    {"EnumNotFound", "Components/Bad.h", 13,
     "the field `Mode` of `FixtureBad` names the enum `Other::FixtureMode`, which the mirror's enum table does not hold under "
     "that name. Fix: declare the enum in a component header or in a module header the component header includes, and write "
     "its name as C++ finds it from the component"},
    {"EnumFoundTwice", "Components/Bad.h", 13,
     "the field `Mode` of `FixtureBad` names the enum `FixtureMode`, which the mirror's enum table holds twice: "
     "{case}/Components/Bad.h:6 and {case}/Components/Twin.h:6. Fix: keep one declaration, or qualify the name"},
    {"EnumWithoutUnderlyingType", "Components/Bad.h", 6,
     "the enum `GameEngine::Components::FixtureMode` has no explicit underlying type. Fix: write one: `enum class FixtureMode : "
     "std::uint8_t`"},
    {"NameCollision", "Components/Bad.h", 6,
     "the C# name `FixturePulse` of `GameEngine::Components::Detail::FixturePulse` is the name of another generated type, "
     "`GameEngine::Components::FixturePulse`. Fix: rename one of them"},
    {"ElementWithoutMirror", "Components/Bad.h", 14,
     "the field `Slots` of `FixtureBad` holds `FixtureSlot`, which is engine-internal and has no mirror. Fix: hold the values "
     "in fields of `FixtureBad`, or take the marker off `FixtureSlot`"},
    {"MemberNamedLikeStruct", "Components/Bad.h", 8,
     "the field `fixtureBad` of `FixtureBad` takes the C# name `FixtureBad`, the name of its struct, which C# refuses for a "
     "member. Fix: rename the field"},
    {"HandWrittenName", "Components/Bad.h", 5,
     "the C# name `Curve` of `GameEngine::Components::Curve` is the name of the hand-written type `Curve`. Fix: rename the C++ "
     "type"},
};

class ComponentMirrorRefusal : public testing::TestWithParam<Refusal>
{
};

std::string Substitute(std::string text, const std::string& from, const std::string& to)
{
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

} // namespace

TEST(ComponentMirrors, MirrorsPinsAndClassesOfTheFixtureAreTheGoldens)
{
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerMirrors"));
    const fs::path csharp = scratch.Path() / "csharp";
    fs::create_directories(csharp / "Components" / "Removed");
    std::ofstream(csharp / "Components" / "Removed" / "Gone.g.cs") << "";
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--api-modules", Utf8Text(kFixture / "Good" / "Modules"), "--api-components", Utf8Text(kFixture / "Good" / "Components"),
         "--emit-api", Utf8Text(scratch.Path() / "api"), "--emit-native", Utf8Text(scratch.Path() / "native"), "--emit-csharp",
         Utf8Text(csharp)},
        out, err);

    ASSERT_EQ(0, exitCode) << err.str();
    EXPECT_EQ(ReadFile(kFixture / "Expected" / "component-classes.txt"), ReadFile(scratch.Path() / "api" / "component-classes.txt"));
    EXPECT_EQ(ReadFile(kFixture / "Expected" / "Components.layout.gen.cpp"),
              ReadFile(scratch.Path() / "native" / "Components.layout.gen.cpp"));
    EXPECT_EQ(CSharpFiles(kFixture / "Expected" / "CSharp"), CSharpFiles(csharp / "Components"));
    EXPECT_NE(std::string::npos, out.str().find("ComponentScanner.components reflected=7 mirrored=3 readonly=1 element=1 internal=1 "
                                                "editor=1 enums=3"))
        << out.str();
}

TEST_P(ComponentMirrorRefusal, NamesTheLineAndTheFixAndWritesNothing)
{
    const Refusal& refusal = GetParam();
    GameEngine::TestUtils::ScopedTempDir scratch(GameEngine::TestUtils::MakeUniqueTempDirectory("ComponentScannerMirrorRefusal"));
    const fs::path tree = kFixture / "Refusals" / refusal.Case;
    std::ostringstream out;
    std::ostringstream err;

    const int exitCode = GameEngine::ComponentScanner::Run(
        {"--api-modules", Utf8Text(kFixture / "Good" / "Modules"), "--api-components", Utf8Text(tree / "Components"), "--emit-api",
         Utf8Text(scratch.Path() / "api"), "--emit-csharp", Utf8Text(scratch.Path() / "csharp")},
        out, err);

    const std::string expected = "error: " + Utf8Text(tree / refusal.File) + ":" + std::to_string(refusal.Line) + ": " +
                                 Substitute(refusal.Message, "{case}", Utf8Text(tree)) + "\n";
    EXPECT_EQ(1, exitCode);
    EXPECT_EQ(expected + "The script API description cannot be generated; nothing was written.\n", err.str());
    EXPECT_FALSE(fs::exists(scratch.Path() / "api"));
    EXPECT_FALSE(fs::exists(scratch.Path() / "csharp"));
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ComponentMirrorRefusal, testing::ValuesIn(kRefusals),
                         [](const testing::TestParamInfo<Refusal>& info) { return std::string(info.param.Case); });
