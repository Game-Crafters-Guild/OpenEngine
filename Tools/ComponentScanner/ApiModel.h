#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The version of the description's format: the "format" of every <Module>.json and the v<N> of
// every generated module table.
inline constexpr std::int64_t kApiDescriptionFormat = 1;

// The kinds a script API declaration may take, return or hold (the binding layer's kind
// table). Each value kind crosses the boundary as the bytes of its C++ type: the integers and
// floats as themselves, Bool as one byte, the Mathematics types as the System.Numerics
// layout, ColorLinear as four floats, Entity as the packed 32-bit ECS::EntityHandle, StringId
// as its 64-bit hash, Angle as a float in radians. String (std::string_view) and Span
// (std::span<const T>) are parameter-only kinds. Record, Enum and Handle name a declaration
// marked in a <Module>ScriptApi.h; Void is a return type only.
enum class ApiKind : std::uint8_t
{
    Void,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Double,
    Angle,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    ColorLinear,
    Entity,
    StringId,
    String,
    Span,
    Record,
    Enum,
    Handle,
};

// The kind's name in the description ("Vec2", "Record").
std::string_view ApiKindName(ApiKind kind);

// A resolved type: its kind, the id of the marked declaration it names for Record, Enum and
// Handle ("Input.ActionState"), and for Span its element type.
struct ApiType
{
    ApiKind Kind = ApiKind::Void;
    std::string TypeId;
    std::vector<ApiType> Element; // one entry for Span, empty otherwise
};

// A default value as the description carries it: the literal text (an enumerator's name for
// an enum, "{}" for a record's own defaults, the resolved literal for a constant), and the id
// of the constant it was written as, when it was.
struct ApiDefault
{
    std::string Literal;
    std::optional<std::string> ConstantId;
};

struct ApiParam
{
    std::string Name;
    ApiType Type;
    bool Out = false;
    std::optional<ApiDefault> Default;
};

// The thread a function may be called on: the main thread (the default), the main thread or a
// job body (@ge-engine-thread), or any thread (@ge-any-thread).
enum class ApiThread : std::uint8_t
{
    Main,
    Engine,
    Any,
};

std::string_view ApiThreadName(ApiThread thread);

// The source location every description entry carries: the header's include path inside its
// module ("Input/InputScriptApi.h") and the 1-based line of the declaration, and for the
// generators' refusals the path the scanner's own refusals print.
struct ApiSource
{
    std::string Header;
    int Line = 0;
    std::string File; // the header's path as the scanner's refusals print it; not in the description
};

struct ApiFunction
{
    std::string Name;
    std::string CppName; // fully qualified, "GameEngine::Input::Script::GetMouseDelta"
    ApiSource Source;
    std::string Doc;
    ApiThread Thread = ApiThread::Main;
    bool Try = false;
    // The @ge-context first parameter's type as written ("InputSystem"); not a parameter of
    // the description: the generated binding supplies it.
    std::optional<std::string> Context;
    ApiType Returns;
    std::vector<ApiParam> Params;
};

struct ApiField
{
    std::string Name;
    ApiType Type;
    std::uint32_t Offset = 0;
    // The member initializer: one literal for a scalar, an enum or a constant, one per
    // component for a Mathematics type or a color; empty for a zero-initialized field or a
    // record field that takes the record's own defaults.
    std::vector<std::string> Default;
    std::optional<std::string> DefaultConstantId;
};

struct ApiRecord
{
    std::string Name;
    std::string CppName;
    ApiSource Source;
    std::string Doc;
    std::uint32_t Size = 0;
    std::uint32_t Align = 1;
    std::vector<ApiField> Fields;
};

struct ApiEnumValue
{
    std::string Name;
    std::int64_t Value = 0;
};

struct ApiEnum
{
    std::string Name;
    std::string CppName;
    ApiSource Source;
    std::string Doc;
    ApiKind Underlying = ApiKind::Int32;
    bool Flags = false; // @ge-script-flags: the values are bits that combine with |
    std::vector<ApiEnumValue> Values;
};

struct ApiHandle
{
    std::string Name;
    std::string CppName;
    ApiSource Source;
    std::string Doc;
    std::uint32_t Width = 64;
};

// When an event's handler runs, and so what it may do.
enum class ApiDelivery : std::uint8_t
{
    // On the main thread, inside PhysicsWorld::Step after the simulation step.
    PhysicsStep,
    // On the main thread, while the UI routes the event.
    UiEventRouting,
};

std::string_view ApiDeliveryName(ApiDelivery delivery);

struct ApiEvent
{
    std::string Name; // "ContactBegin" for SubscribeContactBegin / UnsubscribeContactBegin
    ApiSource Source;  // the Subscribe declaration
    std::string Doc;
    ApiType Scope;     // Entity or a Handle
    std::string PayloadId;
    ApiDelivery Delivery = ApiDelivery::PhysicsStep;
    std::string SubscribeCpp;
    std::string UnsubscribeCpp;
    std::optional<std::string> Context;
};

// A constexpr scalar of a <Module>ScriptApi.h that a default names.
struct ApiConstant
{
    std::string Name;
    std::string CppName;
    ApiSource Source;
    ApiType Type;
    std::string Literal;
};

struct ApiModule
{
    std::string Name;
    std::vector<ApiConstant> Constants;
    std::vector<ApiEnum> Enums;
    std::vector<ApiEvent> Events;
    std::vector<ApiFunction> Functions;
    std::vector<ApiHandle> Handles;
    std::vector<ApiRecord> Records;
};

// The whole description: every module with a marked <Module>ScriptApi.h, by name.
struct ApiDescription
{
    std::vector<ApiModule> Modules;
};

// A declaration the scanner refuses: where it is, what is wrong, and the fix. The scan fails
// when there is any.
struct ApiRefusal
{
    std::string File;
    int Line = 0;
    std::string Problem;
    std::string Fix;
};

} // namespace GameEngine::ComponentScanner
