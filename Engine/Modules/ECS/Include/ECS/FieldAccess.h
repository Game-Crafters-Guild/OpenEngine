#pragma once

// FieldAccess.h — typed reads and writes of one element of a reflected field.
//
// A field is addressed by its FieldInfo inside the bytes of the whole component: element
// `element` sits at FieldInfo::Offset + element * FieldElementSize(Type), and the field holds
// Size / FieldElementSize(Type) elements. Every read and write checks the element against that
// count and the element's bytes against the span, and returns false instead of touching memory
// outside them. A false read leaves `out` unchanged; a false write leaves the bytes unchanged.

#include "ECS/Reflection.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace GameEngine {

class GUID;

namespace ECS {

namespace Detail {

// One copy of a T out of `bytes`, with no alignment requirement. The caller guarantees that
// sizeof(T) bytes are readable.
template <class T>
T ReadUnaligned(const std::byte* bytes)
{
    T value{};
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

// The field kind a C++ value type reads and writes as: the kind reflection gives the type
// (FieldTypeIdOf: bool, float, double, and the FieldTypeTraits composites, ColorLinear and
// EntityHandle here, the Mathematics vectors and Quaternion in Components/MathReflectionTraits.h),
// and AssetGuid for GUID, which reflection leaves opaque. Unknown for integers, enums and arrays:
// integers go through ReadFieldInteger, and an array is a field of elements, not an element.
template <class T>
constexpr FieldTypeId FieldAccessType()
{
    if constexpr (std::is_same_v<T, GUID>)
        return FieldTypeId::AssetGuid;
    else if constexpr ((std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_enum_v<T> ||
                       std::is_array_v<T>)
        return FieldTypeId::Unknown;
    else
        return FieldTypeIdOf<T>();
}

// The byte offset of `element` of `field` in a component of `byteCount` bytes. False when the
// field has no element width, the element is past the field's count, or the element's bytes end
// past the component.
inline bool FieldElementOffset(const FieldInfo& field, std::size_t byteCount, std::uint32_t element,
                               std::size_t& offset)
{
    const std::uint32_t elementSize = FieldElementSize(field.Type);
    if (elementSize == 0 || element >= field.Size / elementSize)
        return false;
    offset = static_cast<std::size_t>(field.Offset) + static_cast<std::size_t>(element) * elementSize;
    return offset + elementSize <= byteCount;
}

// A type ReadFieldElement and WriteFieldElement accept: one with a field kind (integers have none:
// they use ReadFieldInteger and WriteFieldInteger, and a Mathematics type needs
// Components/MathReflectionTraits.h included), copied as bytes, the size of its kind's element.
template <class T>
concept FieldElementType = FieldAccessType<T>() != FieldTypeId::Unknown && std::is_trivially_copyable_v<T> &&
                           FieldElementSize(FieldAccessType<T>()) == sizeof(T);

} // namespace Detail

/// Read element `element` of `field` from the component bytes `bytes` into `out`. T is one of
/// bool, float, double, Vector2, Vector3, Vector4, ColorLinear, Quaternion, GUID or EntityHandle,
/// checked against field.Type. False on a type mismatch or an element outside the field or the
/// span.
template <Detail::FieldElementType T>
bool ReadFieldElement(const FieldInfo& field, std::span<const std::byte> bytes, std::uint32_t element, T& out)
{
    std::size_t offset = 0;
    if (field.Type != Detail::FieldAccessType<T>() ||
        !Detail::FieldElementOffset(field, bytes.size(), element, offset))
    {
        return false;
    }
    if constexpr (std::is_same_v<T, bool>)
        out = bytes[offset] != std::byte{0};
    else
        std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return true;
}

/// Write `value` to element `element` of `field` in the component bytes `bytes`. The same types
/// and refusals as ReadFieldElement; a bool is stored as the byte 1 or 0.
template <Detail::FieldElementType T>
bool WriteFieldElement(const FieldInfo& field, std::span<std::byte> bytes, std::uint32_t element, const T& value)
{
    std::size_t offset = 0;
    if (field.Type != Detail::FieldAccessType<T>() ||
        !Detail::FieldElementOffset(field, bytes.size(), element, offset))
    {
        return false;
    }
    if constexpr (std::is_same_v<T, bool>)
        bytes[offset] = value ? std::byte{1} : std::byte{0};
    else
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    return true;
}

/// Read element `element` of an integer field (Int8 to UInt64) into `out`, with the width and
/// signedness of field.Type. A UInt64 element is returned as its bit pattern, so a value above
/// INT64_MAX reads negative and WriteFieldInteger stores it back exactly. False for a field that
/// is not one of the eight integer kinds, or an element outside the field or the span.
inline bool ReadFieldInteger(const FieldInfo& field, std::span<const std::byte> bytes, std::uint32_t element,
                             std::int64_t& out)
{
    std::size_t offset = 0;
    if (!Detail::FieldElementOffset(field, bytes.size(), element, offset))
        return false;
    const std::byte* source = bytes.data() + offset;
    switch (field.Type)
    {
        case FieldTypeId::Int8:   out = Detail::ReadUnaligned<std::int8_t>(source); return true;
        case FieldTypeId::Int16:  out = Detail::ReadUnaligned<std::int16_t>(source); return true;
        case FieldTypeId::Int32:  out = Detail::ReadUnaligned<std::int32_t>(source); return true;
        case FieldTypeId::Int64:  out = Detail::ReadUnaligned<std::int64_t>(source); return true;
        case FieldTypeId::UInt8:  out = Detail::ReadUnaligned<std::uint8_t>(source); return true;
        case FieldTypeId::UInt16: out = Detail::ReadUnaligned<std::uint16_t>(source); return true;
        case FieldTypeId::UInt32: out = Detail::ReadUnaligned<std::uint32_t>(source); return true;
        case FieldTypeId::UInt64:
            out = static_cast<std::int64_t>(Detail::ReadUnaligned<std::uint64_t>(source));
            return true;
        default: return false;
    }
}

/// Write `value` to element `element` of an integer field (Int8 to UInt64) at the width of
/// field.Type. The low bytes are stored, so a value outside the field's range wraps as a cast
/// does. The same refusals as ReadFieldInteger.
inline bool WriteFieldInteger(const FieldInfo& field, std::span<std::byte> bytes, std::uint32_t element,
                              std::int64_t value)
{
    std::size_t offset = 0;
    if (!Detail::FieldElementOffset(field, bytes.size(), element, offset))
        return false;
    std::byte* destination = bytes.data() + offset;
    const auto store = [destination](auto narrowed) { std::memcpy(destination, &narrowed, sizeof(narrowed)); };
    switch (field.Type)
    {
        case FieldTypeId::Int8:   store(static_cast<std::int8_t>(value)); return true;
        case FieldTypeId::Int16:  store(static_cast<std::int16_t>(value)); return true;
        case FieldTypeId::Int32:  store(static_cast<std::int32_t>(value)); return true;
        case FieldTypeId::Int64:  store(value); return true;
        case FieldTypeId::UInt8:  store(static_cast<std::uint8_t>(value)); return true;
        case FieldTypeId::UInt16: store(static_cast<std::uint16_t>(value)); return true;
        case FieldTypeId::UInt32: store(static_cast<std::uint32_t>(value)); return true;
        case FieldTypeId::UInt64: store(static_cast<std::uint64_t>(value)); return true;
        default: return false;
    }
}

} // namespace ECS
} // namespace GameEngine
