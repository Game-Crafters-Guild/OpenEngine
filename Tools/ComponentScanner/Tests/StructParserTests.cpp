// StructParser operates on comment-stripped source; these fixtures are already comment-free,
// so the parser's line numbers line up with the fixture's own lines.
#include "StructParser.h"

#include <gtest/gtest.h>

using namespace GameEngine::ComponentScanner;

namespace
{

ParsedStruct ParseOne(const char* clean, const std::string& name)
{
    std::vector<SourceWarning> warnings;
    for (ParsedStruct& s : StructParser::Parse(clean, warnings))
        if (s.Name == name)
            return s;
    ADD_FAILURE() << "no struct named " << name;
    return {};
}

std::vector<std::string> Names(const ParsedStruct& s)
{
    std::vector<std::string> names;
    for (const FieldDecl& f : s.Fields)
        names.push_back(f.Name);
    return names;
}

const FieldDecl& Field(const ParsedStruct& s, const std::string& name)
{
    for (const FieldDecl& f : s.Fields)
        if (f.Name == name)
            return f;
    static const FieldDecl kMissing;
    ADD_FAILURE() << "no field named " << name;
    return kMissing;
}

} // namespace

TEST(StructParser, ExtractsScalarAndArrayFieldsInDeclarationOrder)
{
    const char* src = "namespace GameEngine::Components {\n"
                      "struct Probe {\n"
                      "    float X;\n"
                      "    int32 Count;\n"
                      "    char Label[16];\n"
                      "};\n"
                      "}";
    const ParsedStruct s = ParseOne(src, "Probe");
    EXPECT_EQ("GameEngine::Components", s.Namespace);
    EXPECT_EQ((std::vector<std::string>{"X", "Count", "Label"}), Names(s));
    EXPECT_EQ("16", Field(s, "Label").ArrayExtent.value_or(""));
}

TEST(StructParser, RecoversFieldsWithFunctionStyleCastOrCallInitializer)
{
    // Regression guard for the '(' -after- top-level '=' handling (HasTopLevelAssign): a '('
    // inside an initializer is NOT a function parameter list, so the field survives.
    const char* src = "namespace GameEngine::Components {\n"
                      "struct E {\n"
                      "    uint32 Mode = static_cast<uint32>(SomeEnum::Value);\n"
                      "    uint32 Plain = 3u;\n"
                      "    float Made = MakeThing(1, 2);\n"
                      "};\n"
                      "}";
    EXPECT_EQ((std::vector<std::string>{"Mode", "Plain", "Made"}), Names(ParseOne(src, "E")));
}

TEST(StructParser, DoesNotMisparseAssignmentOperatorAsField)
{
    // operator= ends in '=' before '(' but is a function, not an initializer.
    const char* src = "namespace GameEngine::Components {\n"
                      "struct WithOp {\n"
                      "    float X;\n"
                      "    WithOp& operator=(const WithOp& other);\n"
                      "    bool operator==(const WithOp& other) const;\n"
                      "};\n"
                      "}";
    const ParsedStruct s = ParseOne(src, "WithOp");
    EXPECT_EQ((std::vector<std::string>{"X"}), Names(s));
    EXPECT_FALSE(s.HasVirtual);
    EXPECT_FALSE(s.UnexpandedMacro.has_value());
}

TEST(StructParser, ExtractsCommaSeparatedDeclarators)
{
    const char* src = "namespace GameEngine::Components {\n"
                      "struct C {\n"
                      "    int32 A, B, Cc;\n"
                      "};\n"
                      "}";
    EXPECT_EQ((std::vector<std::string>{"A", "B", "Cc"}), Names(ParseOne(src, "C")));
}

TEST(StructParser, FlagsUnexpandedFieldInjectionMacro)
{
    // A bare ALL-CAPS macro token injects fields the scanner can't see -> recorded so the
    // caller skips the struct (mirrors the terrain modifiers' GE_TERRAIN_EFFECT_COMMON_FIELDS).
    const char* src = "namespace GameEngine::Components {\n"
                      "struct M {\n"
                      "    GE_SOME_COMMON_FIELDS\n"
                      "    float Extra;\n"
                      "};\n"
                      "}";
    EXPECT_EQ("GE_SOME_COMMON_FIELDS", ParseOne(src, "M").UnexpandedMacro.value_or(""));
}

TEST(StructParser, DetectsVirtualMember)
{
    const char* src = "namespace GameEngine::Components {\n"
                      "struct V {\n"
                      "    float X;\n"
                      "    virtual void Tick();\n"
                      "};\n"
                      "}";
    EXPECT_TRUE(ParseOne(src, "V").HasVirtual);
}

TEST(StructParser, SkipsMethodsAndStaticsButKeepsDataMembers)
{
    const char* src = "namespace GameEngine::Components {\n"
                      "struct Mixed {\n"
                      "    static constexpr int Cap = 8;\n"
                      "    int32 Count;\n"
                      "    void Reset() { Count = 0; }\n"
                      "    float Values[4];\n"
                      "};\n"
                      "}";
    EXPECT_EQ((std::vector<std::string>{"Count", "Values"}), Names(ParseOne(src, "Mixed")));
}

TEST(StructParser, RecordsFieldDeclarationLineNumbers)
{
    // Lines: 1 namespace, 2 struct, 3 First, 4 Second, 5 '}'.
    const char* src = "namespace GameEngine::Components {\nstruct L {\n    float First;\n    int32 Second;\n};\n}";
    const ParsedStruct s = ParseOne(src, "L");
    EXPECT_EQ(2, s.DeclLine);
    EXPECT_EQ(3, Field(s, "First").DeclLine);
    EXPECT_EQ(4, Field(s, "Second").DeclLine);
}

TEST(StructParser, SkipsTemplateStructDefinitions)
{
    // A `template<...> struct` is a template, not a component — registering it would emit
    // GE_REGISTER_COMPONENT on an unspecialized template (won't compile). A concrete sibling
    // declared right after it must still parse.
    const char* src = "namespace GameEngine::Components {\n"
                      "template<AssetType kType = AssetType::Unknown>\n"
                      "struct AssetRef {\n"
                      "    uint8 Bytes[16];\n"
                      "};\n"
                      "struct Concrete {\n"
                      "    float X;\n"
                      "};\n"
                      "}";
    std::vector<SourceWarning> warnings;
    const std::vector<ParsedStruct> all = StructParser::Parse(src, warnings);
    bool sawAssetRef = false;
    bool sawConcrete = false;
    for (const ParsedStruct& s : all)
    {
        sawAssetRef = sawAssetRef || s.Name == "AssetRef";
        sawConcrete = sawConcrete || s.Name == "Concrete";
    }
    EXPECT_FALSE(sawAssetRef);
    EXPECT_TRUE(sawConcrete);
}

TEST(StructParser, RecordsMultiLineInitializerEndLine)
{
    // A brace initializer spanning lines: DeclLine at the head, EndLine at the ';'.
    const char* src = "namespace GameEngine::Components {\nstruct A {\n    float Axis[3] = {0.0f,\n                    1.0f,\n                    0.0f};\n};\n}";
    const ParsedStruct s = ParseOne(src, "A");
    const FieldDecl& axis = Field(s, "Axis");
    EXPECT_EQ(3, axis.DeclLine);
    EXPECT_EQ(5, axis.EndLine);
}
