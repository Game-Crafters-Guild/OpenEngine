#include "EnumScanner.h"
#include "ReflectedModel.h"

#include <gtest/gtest.h>

using namespace GameEngine::ComponentScanner;

namespace
{

EnumDecl One(const char* src, const std::string& name)
{
    for (EnumDecl& e : EnumScanner::Scan(src))
        if (e.Name == name)
            return e;
    ADD_FAILURE() << "no enum named " << name;
    return {};
}

std::vector<std::int64_t> Values(const EnumDecl& e)
{
    std::vector<std::int64_t> values;
    for (const EnumMember& m : e.Members)
        values.push_back(m.Value);
    return values;
}

std::vector<std::string> Names(const EnumDecl& e)
{
    std::vector<std::string> names;
    for (const EnumMember& m : e.Members)
        names.push_back(m.Name);
    return names;
}

} // namespace

TEST(EnumScanner, ParsesImplicitAndExplicitValues)
{
    const EnumDecl e = One("enum class Mode : uint32 { Off = 0, Low = 1, High = 2, Turbo = 7 };", "Mode");
    ASSERT_EQ(4u, e.Members.size());
    EXPECT_EQ("Off", e.Members[0].Name);
    EXPECT_EQ(0, e.Members[0].Value);
    EXPECT_EQ("Turbo", e.Members[3].Name);
    EXPECT_EQ(7, e.Members[3].Value);
}

TEST(EnumScanner, ImplicitValuesAutoIncrementFromPrevious)
{
    // Mix: implicit 0, explicit 5, then implicit 6, 7.
    const EnumDecl e = One("enum class E { A, B = 5, C, D };", "E");
    EXPECT_EQ((std::vector<std::int64_t>{0, 5, 6, 7}), Values(e));
    EXPECT_EQ((std::vector<std::string>{"A", "B", "C", "D"}), Names(e));
}

TEST(EnumScanner, ParsesHexValues)
{
    const EnumDecl e = One("enum class Flags : uint32 { None = 0x0, A = 0x10, B = 0xFF };", "Flags");
    EXPECT_EQ((std::vector<std::int64_t>{0, 16, 255}), Values(e));
}

TEST(EnumScanner, ParsesUnderlyingTypeAndMultiLineWithTrailingComma)
{
    const char* src = "enum class Shape : uint8_t\n"
                      "{\n"
                      "    Box      = 0,\n"
                      "    Sphere   = 1,\n"
                      "    Capsule  = 2,\n"
                      "};";
    const EnumDecl e = One(src, "Shape");
    ASSERT_EQ(3u, e.Members.size());
    EXPECT_EQ("Capsule", e.Members[2].Name);
    EXPECT_EQ(2, e.Members[2].Value);
    EXPECT_EQ("uint8_t", e.Underlying.value_or(""));
}

TEST(EnumScanner, HandlesPlainEnumAndImplicitUnderlyingType)
{
    const EnumDecl e = One("enum Direction { North, East, South, West };", "Direction");
    EXPECT_EQ((std::vector<std::string>{"North", "East", "South", "West"}), Names(e));
    EXPECT_EQ((std::vector<std::int64_t>{0, 1, 2, 3}), Values(e));
    EXPECT_FALSE(e.Underlying.has_value());
}

TEST(EnumScanner, StripsIntegerSuffixes)
{
    const EnumDecl e = One("enum class E : uint32 { A = 1u, B = 2ul };", "E");
    EXPECT_EQ((std::vector<std::int64_t>{1, 2}), Values(e));
}

TEST(EnumScanner, BailsOnExpressionValues)
{
    // A bit-shift value can't be evaluated — the enum is dropped (its fields stay integers).
    EXPECT_TRUE(EnumScanner::Scan("enum class Flags : uint32 { A = 1 << 0, B = 1 << 1 };").empty());
}

TEST(EnumScanner, BailsOnValuesPastTheInt64Range)
{
    // An implicit enumerator after the int64 maximum, and the negated most negative value,
    // have no int64: the enum is dropped instead of wrapping around.
    EXPECT_TRUE(EnumScanner::Scan("enum class Big : int64 { Max = 9223372036854775807, After };").empty());
    EXPECT_TRUE(EnumScanner::Scan("enum class Low : int64 { Min = -0x8000000000000000 };").empty());
}

TEST(EnumScanner, IgnoresForwardDeclarationWithoutBody)
{
    EXPECT_TRUE(EnumScanner::Scan("enum class Opaque : uint32;").empty());
}

TEST(EnumScanner, DoesNotMatchEnumSubstringOfIdentifier)
{
    // `scoped_enum` / `my_enum_t` must not be parsed as an `enum` declaration.
    EXPECT_TRUE(EnumScanner::Scan("struct my_enum_t { int scoped_enum_value; };").empty());
}

TEST(EnumScanner, MatchEnumStripsQualifiersAndNamespace)
{
    EnumTable table;
    table.ByName["Bar"] = EnumDecl{"Bar", {EnumMember{"A", 0}}, std::nullopt};
    const EnumDecl* bar = &table.ByName["Bar"];

    EXPECT_EQ(bar, table.Match("Bar"));
    EXPECT_EQ(bar, table.Match("Foo::Bar"));
    EXPECT_EQ(bar, table.Match("const Bar"));
    EXPECT_EQ(nullptr, table.Match("uint32"));
}

TEST(EnumScanner, MatchEnumReturnsNullForAmbiguous)
{
    EnumTable table;
    table.ByName["Shape"] = EnumDecl{"Shape", {EnumMember{"A", 0}}, std::nullopt};
    table.Ambiguous.insert("Shape");
    EXPECT_EQ(nullptr, table.Match("Shape"));
}
