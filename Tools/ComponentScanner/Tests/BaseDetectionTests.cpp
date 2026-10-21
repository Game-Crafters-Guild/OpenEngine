// Covers the inheritance-based component detection (--detect-base ComponentBase), used to
// auto-register user components declared as `struct X : ECS::ComponentBase { ... }` with no
// macro. Detection = the struct's base-clause has a base whose simple name matches.
#include "ComponentScanner.h"
#include "StructParser.h"

#include <gtest/gtest.h>

using namespace GameEngine::ComponentScanner;

TEST(BaseDetection, InheritsBaseMatchesSimpleName)
{
    struct Case
    {
        const char* Bases;
        bool Expected;
    };
    const Case cases[] = {
        {"public ECS::ComponentBase", true},
        {"ECS::ComponentBase", true},
        {"GameEngine::ECS::ComponentBase", true},
        {"ComponentBase", true},
        {"public virtual ECS::ComponentBase", true},
        {"public A, public ECS::ComponentBase", true}, // second of two bases
        {"", false},
        {"public SomethingElse", false},
        {"public ECS::Component", false}, // the concept name must NOT match the tag
        {"public std::enable_shared_from_this<Foo>", false},
    };
    for (const Case& c : cases)
        EXPECT_EQ(c.Expected, InheritsBase(c.Bases, "ComponentBase")) << c.Bases;
}

TEST(BaseDetection, StructParserCapturesBaseClause)
{
    std::vector<SourceWarning> warnings;
    const std::vector<ParsedStruct> structs =
        StructParser::Parse("struct Health : public ECS::ComponentBase { float Current; int Lives; };", warnings);
    ASSERT_EQ(1u, structs.size());
    EXPECT_EQ("Health", structs[0].Name);
    EXPECT_NE(std::string::npos, structs[0].Bases.find("ComponentBase"));
    EXPECT_EQ(2u, structs[0].Fields.size()); // Current, Lives
}

TEST(BaseDetection, PlainStructHasNoBases)
{
    std::vector<SourceWarning> warnings;
    const std::vector<ParsedStruct> structs = StructParser::Parse("struct Plain { float X; };", warnings);
    ASSERT_EQ(1u, structs.size());
    EXPECT_EQ("", structs[0].Bases);
}

// InheritingStructNames backs the .cpp diagnostic: a macro-free component authored in a .cpp
// (not a header) can't be reflected, so the scanner names it for a build warning.
TEST(BaseDetection, InheritingStructNamesFindsInheritorsForWarning)
{
    const char* src = "namespace game { struct Speed : ECS::ComponentBase { float V; }; }\n"
                      "struct Plain { int X; };\n"
                      "struct Mana : public ECS::ComponentBase { float M; };";
    EXPECT_EQ((std::vector<std::string>{"Speed", "Mana"}), InheritingStructNames(src, "ComponentBase"));
}

TEST(BaseDetection, InheritingStructNamesEmptyWhenNoneInherit)
{
    EXPECT_TRUE(InheritingStructNames("struct A { int X; }; struct B : public Other { int Y; };", "ComponentBase").empty());
}
