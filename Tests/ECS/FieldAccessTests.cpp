// ECS::FieldAccess: the typed read and write of one element of a reflected field. Each test
// builds the FieldInfo a component's table would carry over a plain byte buffer, so a failure
// names the accessor and not a component's layout.

#include "ECS/FieldAccess.h"

#include "AssetCore/GUID.h"
#include "Components/MathReflectionTraits.h"  // Vector2/3/4 and Quaternion field kinds
#include "ECS/ECS.h"                          // EntityHandle (complete)
#include "Types/Color.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace
{

constexpr std::uint32_t kFieldOffset = 4;

FieldInfo MakeField(FieldTypeId type, std::uint32_t count = 1)
{
    return FieldInfo{"Probe", kFieldOffset, FieldElementSize(type) * count, type};
}

std::vector<std::byte> MakeComponentBytes(const FieldInfo& field)
{
    return std::vector<std::byte>(field.Offset + field.Size, std::byte{0xAB});
}

template <class T>
void ExpectRoundTrip(FieldTypeId type, const T& value)
{
    const FieldInfo field = MakeField(type, 2);
    std::vector<std::byte> bytes = MakeComponentBytes(field);
    ASSERT_TRUE(WriteFieldElement(field, std::span<std::byte>(bytes), 1, value));
    T read{};
    ASSERT_TRUE(ReadFieldElement(field, std::span<const std::byte>(bytes), 1, read));
    EXPECT_EQ(0, std::memcmp(&read, &value, sizeof(T)));
    EXPECT_EQ(0, std::memcmp(bytes.data() + field.Offset + FieldElementSize(type), &value, sizeof(T)));
    // Element 0 and the bytes before the field are untouched.
    EXPECT_EQ(bytes[0], std::byte{0xAB});
    EXPECT_EQ(bytes[field.Offset], std::byte{0xAB});
}

} // namespace

TEST(FieldAccess, EachElementKindRoundTripsAtItsElement)
{
    ExpectRoundTrip(FieldTypeId::Float, 2.5f);
    ExpectRoundTrip(FieldTypeId::Double, -7.25);
    ExpectRoundTrip(FieldTypeId::Vec2, Mathematics::Vector2(1.0f, 2.0f));
    ExpectRoundTrip(FieldTypeId::Vec3, Mathematics::Vector3(1.0f, 2.0f, 3.0f));
    ExpectRoundTrip(FieldTypeId::Vec4, Mathematics::Vector4(1.0f, 2.0f, 3.0f, 4.0f));
    ExpectRoundTrip(FieldTypeId::Color, ColorLinear(0.25f, 0.5f, 0.75f, 1.0f));
    ExpectRoundTrip(FieldTypeId::Quat, Mathematics::Quaternion(0.0f, 0.0f, 0.70710677f, 0.70710677f));
    GUID::Data guidBytes{};
    for (std::size_t i = 0; i < guidBytes.size(); ++i)
        guidBytes[i] = static_cast<std::uint8_t>(i + 1);
    ExpectRoundTrip(FieldTypeId::AssetGuid, GUID(guidBytes));
    EntityHandle handle{};
    handle.id = 0x01020304u;
    ExpectRoundTrip(FieldTypeId::EntityHandle, handle);
}

TEST(FieldAccess, BoolIsStoredAsOneOrZeroAndReadAsNonZero)
{
    const FieldInfo field = MakeField(FieldTypeId::Bool);
    std::vector<std::byte> bytes = MakeComponentBytes(field);
    bool value = false;
    ASSERT_TRUE(ReadFieldElement(field, std::span<const std::byte>(bytes), 0, value));
    EXPECT_TRUE(value);  // 0xAB is true
    ASSERT_TRUE(WriteFieldElement(field, std::span<std::byte>(bytes), 0, true));
    EXPECT_EQ(bytes[field.Offset], std::byte{1});
    ASSERT_TRUE(WriteFieldElement(field, std::span<std::byte>(bytes), 0, false));
    EXPECT_EQ(bytes[field.Offset], std::byte{0});
}

TEST(FieldAccess, IntegersKeepTheWidthAndSignednessOfTheirType)
{
    struct Case
    {
        FieldTypeId Type;
        std::int64_t ReadOfMinusOne;
    };
    const std::array<Case, 8> cases{{
        {FieldTypeId::Int8, -1},
        {FieldTypeId::Int16, -1},
        {FieldTypeId::Int32, -1},
        {FieldTypeId::Int64, -1},
        {FieldTypeId::UInt8, 0xFF},
        {FieldTypeId::UInt16, 0xFFFF},
        {FieldTypeId::UInt32, 0xFFFFFFFFll},
        {FieldTypeId::UInt64, -1},  // the bit pattern of UINT64_MAX
    }};
    for (const Case& entry : cases)
    {
        const FieldInfo field = MakeField(entry.Type, 2);
        // Eight bytes past the field, so a store wider than the element shows past its end too.
        std::vector<std::byte> bytes(field.Offset + field.Size + 8, std::byte{0});
        ASSERT_TRUE(WriteFieldInteger(field, std::span<std::byte>(bytes), 1, -1));
        std::int64_t value = 0;
        ASSERT_TRUE(ReadFieldInteger(field, std::span<const std::byte>(bytes), 1, value));
        EXPECT_EQ(value, entry.ReadOfMinusOne) << static_cast<int>(entry.Type);
        // Only the element's own width was written.
        const std::uint32_t width = FieldElementSize(entry.Type);
        for (std::uint32_t index = 0; index < bytes.size(); ++index)
        {
            const bool inElement = index >= field.Offset + width && index < field.Offset + 2 * width;
            EXPECT_EQ(bytes[index], inElement ? std::byte{0xFF} : std::byte{0}) << static_cast<int>(entry.Type);
        }
    }
}

TEST(FieldAccess, UInt64KeepsTheFullRange)
{
    const FieldInfo field = MakeField(FieldTypeId::UInt64);
    std::vector<std::byte> bytes = MakeComponentBytes(field);
    constexpr std::uint64_t kHigh = 0x8000000000000001ull;
    ASSERT_TRUE(WriteFieldInteger(field, std::span<std::byte>(bytes), 0, static_cast<std::int64_t>(kHigh)));
    std::uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + field.Offset, sizeof(stored));
    EXPECT_EQ(stored, kHigh);
    std::int64_t value = 0;
    ASSERT_TRUE(ReadFieldInteger(field, std::span<const std::byte>(bytes), 0, value));
    EXPECT_EQ(static_cast<std::uint64_t>(value), kHigh);
}

TEST(FieldAccess, AnIntegerOutsideTheFieldRangeWrapsAsACastDoes)
{
    const FieldInfo field = MakeField(FieldTypeId::Int16);
    std::vector<std::byte> bytes = MakeComponentBytes(field);
    constexpr std::int64_t kAboveInt16 = 70000;
    ASSERT_TRUE(WriteFieldInteger(field, std::span<std::byte>(bytes), 0, kAboveInt16));
    std::int64_t value = 0;
    ASSERT_TRUE(ReadFieldInteger(field, std::span<const std::byte>(bytes), 0, value));
    EXPECT_EQ(value, static_cast<std::int16_t>(kAboveInt16));
}

TEST(FieldAccess, AWrongTypeIsRefusedAndNothingIsWritten)
{
    const FieldInfo doubleField = MakeField(FieldTypeId::Double);
    std::vector<std::byte> bytes = MakeComponentBytes(doubleField);
    const std::vector<std::byte> before = bytes;
    float floatValue = 1.0f;
    EXPECT_FALSE(ReadFieldElement(doubleField, std::span<const std::byte>(bytes), 0, floatValue));
    EXPECT_EQ(floatValue, 1.0f);
    EXPECT_FALSE(WriteFieldElement(doubleField, std::span<std::byte>(bytes), 0, 2.0f));
    std::int64_t integer = 7;
    EXPECT_FALSE(ReadFieldInteger(doubleField, std::span<const std::byte>(bytes), 0, integer));
    EXPECT_EQ(integer, 7);
    EXPECT_FALSE(WriteFieldInteger(doubleField, std::span<std::byte>(bytes), 0, 3));

    const FieldInfo intField = MakeField(FieldTypeId::Int32);
    EXPECT_FALSE(WriteFieldElement(intField, std::span<std::byte>(bytes), 0, true));
    EXPECT_EQ(bytes, before);
    EXPECT_TRUE(WriteFieldElement(doubleField, std::span<std::byte>(bytes), 0, 2.0));
}

TEST(FieldAccess, AnElementPastTheCountOrTheSpanIsRefused)
{
    const FieldInfo field = MakeField(FieldTypeId::Float, 2);
    std::vector<std::byte> bytes = MakeComponentBytes(field);
    const std::vector<std::byte> before = bytes;
    float value = 0.0f;
    EXPECT_FALSE(ReadFieldElement(field, std::span<const std::byte>(bytes), 2, value));
    EXPECT_FALSE(WriteFieldElement(field, std::span<std::byte>(bytes), 2, 1.0f));
    std::int64_t integer = 0;
    const FieldInfo intField = MakeField(FieldTypeId::UInt16, 2);
    EXPECT_FALSE(ReadFieldInteger(intField, std::span<const std::byte>(bytes), 2, integer));
    EXPECT_FALSE(WriteFieldInteger(intField, std::span<std::byte>(bytes), 2, 1));

    // A span that ends inside element 1 still serves element 0.
    const std::span<std::byte> shortSpan(bytes.data(), field.Offset + field.Size - 1);
    EXPECT_FALSE(WriteFieldElement(field, shortSpan, 1, 1.0f));
    EXPECT_FALSE(ReadFieldElement(field, std::span<const std::byte>(shortSpan), 1, value));
    EXPECT_EQ(bytes, before);
    EXPECT_TRUE(WriteFieldElement(field, shortSpan, 0, 1.0f));
}
