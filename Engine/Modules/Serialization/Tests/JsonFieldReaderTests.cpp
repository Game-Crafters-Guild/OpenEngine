#include <gtest/gtest.h>

#include "Serialization/JsonRead.h"

using namespace GameEngine;
using namespace GameEngine::Serialization;

// A Required vector or rotation field that is absent is refused with the field named; an Optional
// one gives the fallback with no refusal.
TEST(JsonFieldReader, ARequiredFieldThatIsAbsentIsRefusedAndAnOptionalOneGivesTheFallback)
{
    const nlohmann::json object = nlohmann::json::parse(R"({"position": [1, 2, 3]})");

    JsonFieldReader optional;
    const Mathematics::Vector3 fallback(4.0f, 5.0f, 6.0f);
    EXPECT_EQ(optional.ReadVector3(object, "scale", fallback).x, 4.0f);
    EXPECT_FALSE(optional.Refused());

    JsonFieldReader required;
    required.SetContext("player.");
    EXPECT_EQ(required.ReadVector3(object, "position", fallback, FieldPresence::Required).y, 2.0f);
    EXPECT_FALSE(required.Refused());
    EXPECT_EQ(required.ReadVector3(object, "scale", fallback, FieldPresence::Required).z, 6.0f);
    EXPECT_EQ(required.Refusal(), "'player.scale' is missing; write it as [x, y, z]");

    JsonFieldReader requiredRotation;
    requiredRotation.ReadQuaternion(object, "rotation", Mathematics::Quaternion::Identity(), FieldPresence::Required);
    EXPECT_EQ(requiredRotation.Refusal(), "'rotation' is missing; write it as [w, x, y, z]");
}
