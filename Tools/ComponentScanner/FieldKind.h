#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

namespace GameEngine::ComponentScanner
{

// The scanner's mirror of ECS::FieldTypeId (Engine/Modules/ECS/Include/ECS/Reflection.h):
// the kind a reflected field registers as, resolved from the field's C++ type text. Same
// names and values as the C++ enum; the web emitters write these names into the generated
// JSON so it compares field by field with the live registry.
enum class FieldKind : std::uint16_t
{
    Unknown = 0,
    Bool = 1,
    Int8 = 2,
    Int16 = 3,
    Int32 = 4,
    Int64 = 5,
    UInt8 = 6,
    UInt16 = 7,
    UInt32 = 8,
    UInt64 = 9,
    Float = 10,
    Double = 11,
    Vec2 = 12,
    Vec3 = 13,
    Vec4 = 14,
    Quat = 15,
    Mat4 = 16,
    Color = 17,
    AssetGuid = 18,
    EntityHandle = 19,
    String = 20,
    Bytes = 21,
    RenderLayerMask = 22,
};

// Every kind with its name, in value order: the JSON spelling and the table the test
// compares against the engine's enum.
inline constexpr std::pair<std::string_view, FieldKind> kFieldKinds[] = {
    {"Unknown", FieldKind::Unknown},   {"Bool", FieldKind::Bool},     {"Int8", FieldKind::Int8},
    {"Int16", FieldKind::Int16},       {"Int32", FieldKind::Int32},   {"Int64", FieldKind::Int64},
    {"UInt8", FieldKind::UInt8},       {"UInt16", FieldKind::UInt16}, {"UInt32", FieldKind::UInt32},
    {"UInt64", FieldKind::UInt64},     {"Float", FieldKind::Float},   {"Double", FieldKind::Double},
    {"Vec2", FieldKind::Vec2},         {"Vec3", FieldKind::Vec3},     {"Vec4", FieldKind::Vec4},
    {"Quat", FieldKind::Quat},         {"Mat4", FieldKind::Mat4},     {"Color", FieldKind::Color},
    {"AssetGuid", FieldKind::AssetGuid}, {"EntityHandle", FieldKind::EntityHandle},
    {"String", FieldKind::String},     {"Bytes", FieldKind::Bytes},   {"RenderLayerMask", FieldKind::RenderLayerMask},
};

inline std::string_view FieldKindName(FieldKind kind)
{
    for (const auto& [name, value] : kFieldKinds)
        if (value == kind)
            return name;
    return "Unknown";
}

} // namespace GameEngine::ComponentScanner
