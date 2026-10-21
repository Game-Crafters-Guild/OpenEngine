#include "ReflectionEmitter.h"

#include <gtest/gtest.h>

using namespace GameEngine::ComponentScanner;

// A control character in a tooltip is written as a three-digit octal escape, the only
// numeric form a C++ string literal reads that way: decimal digits would turn ESC (27)
// into "\027", which the compiler reads as ETB (23).
TEST(ReflectionEmitter, EscapesTooltipControlCharactersAsOctal)
{
    ReflectedField field;
    field.Name = "Speed";
    field.Type = "float";
    field.Tooltip = std::string("a\x1b" "b\x7f");

    ReflectedStruct component;
    component.Name = "Mover";
    component.Header = "Mover.h";
    component.Fields.push_back(field);

    const std::string cpp = ReflectionEmitter::EmitGeneratedCpp({component}, {"Mover.h"}, true, EnumTable{});
    EXPECT_NE(std::string::npos, cpp.find(R"(GE_REFLECT_FIELD_TOOLTIP(Mover, Speed, "a\033b\177");)")) << cpp;
}
