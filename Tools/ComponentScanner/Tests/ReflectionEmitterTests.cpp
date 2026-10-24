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

// A struct-level @ge-readonly or @ge-engine-internal registers the component as one a script may not write,
// with the system that writes it; a tag (no fields) registers with an empty field table.
TEST(ReflectionEmitter, RegistersTheScriptWriteRuleAndATag)
{
    ReflectedField field;
    field.Name = "Elapsed";
    field.Type = "float";
    ReflectedStruct clock;
    clock.Name = "Clock";
    clock.Header = "Clock.h";
    clock.ReadOnly = true;
    clock.Writer = "ClockSystem";
    clock.Fields.push_back(field);
    ReflectedStruct slot = clock;
    slot.Name = "Slot";
    slot.ReadOnly = false;
    slot.EngineInternal = true;
    slot.Writer = "render extraction";
    ReflectedStruct tag;
    tag.Name = "PickedUp";
    tag.Header = "Clock.h";

    const std::string cpp = ReflectionEmitter::EmitGeneratedCpp({clock, slot, tag}, {"Clock.h"}, true, EnumTable{});

    EXPECT_NE(std::string::npos, cpp.find(R"(GE_REFLECT_COMPONENT_ENGINE_OWNED(Clock, "ClockSystem");)")) << cpp;
    EXPECT_NE(std::string::npos, cpp.find(R"(GE_REFLECT_COMPONENT_ENGINE_INTERNAL(Slot, "render extraction");)")) << cpp;
    EXPECT_NE(std::string::npos, cpp.find("GE_REGISTER_TAG_COMPONENT(PickedUp)\n")) << cpp;
    EXPECT_EQ(std::string::npos, cpp.find("GE_REGISTER_COMPONENT_BEGIN(PickedUp)")) << cpp;
}
