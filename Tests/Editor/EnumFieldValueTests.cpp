// set_component's enum values: an enumerator name or a declared integer; an integer the enum does not
// declare is refused with a message that lists the declared names and values.

#include "DebugServer/Components/FieldValueReaders.h"

#include "ECS/Reflection.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

using GameEngine::ECS::EnumNameValue;
using GameEngine::ECS::FieldInfo;
using GameEngine::ECS::FieldTypeId;
using GameEngine::Editor::ReadEnumFieldValue;
using nlohmann::json;

namespace
{
// A UInt8 enum with a gap, shaped like ValueCurve.Target: 0 to 2 and 8 are declared.
constexpr std::array<EnumNameValue, 4> kGappedTargets{{
    {"None", 0},
    {"TransformPositionX", 1},
    {"TransformPositionY", 2},
    {"PostProcessWeight", 8},
}};

FieldInfo GappedTargetField()
{
    FieldInfo field{"Target", 0, 1, FieldTypeId::UInt8};
    field.EnumNames = kGappedTargets;
    return field;
}
} // namespace

TEST(EnumFieldValueTest, AnUndeclaredIntegerIsRefusedWithTheDeclaredValues)
{
    const FieldInfo field = GappedTargetField();
    for (const json& value : {json(9), json(200), json(-1), json("9"), json(1.5)})
    {
        std::int64_t out = 2;
        std::string error;
        EXPECT_FALSE(ReadEnumFieldValue(value, field, out, error)) << value.dump();
        EXPECT_EQ(out, 2) << "a refused value leaves the output alone";
        EXPECT_NE(error.find("Target takes one of None=0, TransformPositionX=1, TransformPositionY=2, "
                             "PostProcessWeight=8; " + value.dump() + " is not one of them"),
                  std::string::npos)
            << error;
    }
}

TEST(EnumFieldValueTest, ADeclaredIntegerIsTakenAsANumberOrAString)
{
    const FieldInfo field = GappedTargetField();
    std::int64_t out = 0;
    std::string error;
    ASSERT_TRUE(ReadEnumFieldValue(json(8), field, out, error)) << error;
    EXPECT_EQ(out, 8);
    ASSERT_TRUE(ReadEnumFieldValue(json("2"), field, out, error)) << error;
    EXPECT_EQ(out, 2);
}

TEST(EnumFieldValueTest, ANameMatchesCaseInsensitivelyAndAWrongNameIsRefused)
{
    const FieldInfo field = GappedTargetField();
    std::int64_t out = 0;
    std::string error;
    ASSERT_TRUE(ReadEnumFieldValue(json("postprocessweight"), field, out, error)) << error;
    EXPECT_EQ(out, 8);
    EXPECT_FALSE(ReadEnumFieldValue(json("NoSuchTarget"), field, out, error));
    EXPECT_NE(error.find("\"NoSuchTarget\" is not one of them"), std::string::npos) << error;
}

TEST(EnumFieldValueTest, APartOfANameOrAValueOfAnotherShapeIsRefused)
{
    const FieldInfo field = GappedTargetField();
    for (const json& value : {json("Transform"), json(""), json(" 2"), json("+2"), json("0x2"), json(2.0), json(true),
                              json(nullptr), json::array({2}), json::object(), json(18446744073709551615ull)})
    {
        std::int64_t out = 1;
        std::string error;
        EXPECT_FALSE(ReadEnumFieldValue(value, field, out, error)) << value.dump();
        EXPECT_EQ(out, 1) << value.dump();
    }
}
