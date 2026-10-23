#include "ApiModel.h"

namespace GameEngine::ComponentScanner
{

std::string_view ApiKindName(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Void: return "Void";
    case ApiKind::Bool: return "Bool";
    case ApiKind::Int8: return "Int8";
    case ApiKind::Int16: return "Int16";
    case ApiKind::Int32: return "Int32";
    case ApiKind::Int64: return "Int64";
    case ApiKind::UInt8: return "UInt8";
    case ApiKind::UInt16: return "UInt16";
    case ApiKind::UInt32: return "UInt32";
    case ApiKind::UInt64: return "UInt64";
    case ApiKind::Float: return "Float";
    case ApiKind::Double: return "Double";
    case ApiKind::Angle: return "Angle";
    case ApiKind::Vec2: return "Vec2";
    case ApiKind::Vec3: return "Vec3";
    case ApiKind::Vec4: return "Vec4";
    case ApiKind::Quat: return "Quat";
    case ApiKind::Mat4: return "Mat4";
    case ApiKind::ColorLinear: return "ColorLinear";
    case ApiKind::Entity: return "Entity";
    case ApiKind::RenderLayerMask: return "RenderLayerMask";
    case ApiKind::StringId: return "StringId";
    case ApiKind::String: return "String";
    case ApiKind::Span: return "Span";
    case ApiKind::Record: return "Record";
    case ApiKind::Enum: return "Enum";
    case ApiKind::Handle: return "Handle";
    }
    return "Void";
}

std::string_view ApiThreadName(ApiThread thread)
{
    switch (thread)
    {
    case ApiThread::Main: return "main";
    case ApiThread::Engine: return "engine";
    case ApiThread::Any: return "any";
    }
    return "main";
}

std::string_view ApiDeliveryName(ApiDelivery delivery)
{
    switch (delivery)
    {
    case ApiDelivery::PhysicsStep: return "PhysicsStep";
    case ApiDelivery::UiEventRouting: return "UiEventRouting";
    }
    return "PhysicsStep";
}

} // namespace GameEngine::ComponentScanner
