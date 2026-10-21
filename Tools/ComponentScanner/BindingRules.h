#pragma once

#include "ApiModel.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The binding layer's rule set, in one place: which kinds the generators emit, how each kind
// crosses the C boundary, and every name a generated binding carries (the native table, its
// thunks and exports; the C# module class, its methods, parameters, records, fields and enums).
// NativeBindingEmitter and CSharpBindingEmitter take every type and every name from here, so a
// rename is one edit in this file.
//
// The crossing. Every thunk returns GE_Result and takes its arguments in the description's
// order, then one pointer for the return value when the function returns one:
//   Bool            std::uint8_t (0 or 1)                  C# byte
//   Int8 .. UInt64  the fixed-width integer                C# sbyte .. ulong
//   Float, Angle    float (Angle in radians)               C# float
//   Double          double                                 C# double
//   StringId        std::uint64_t, 0 refused               C# ulong (the public type is StringId)
//   Enum            the enum type (its underlying type)    C# the enum
//   Vec2 .. Mat4    const T* in, T* out                    C# System.Numerics Vector2 .. Matrix4x4
//   Record          const T* in, T* out                    C# the record, explicit layout
//   Entity          std::uint32_t, the packed EntityHandle C# uint (the public type is GameEngine.Entity)
// An out parameter and the return value cross as a pointer to the value; a Bool as a pointer
// to one byte, an Entity as a pointer to its packed id. A null pointer is refused with
// GE_Result_InvalidArg.
//
// An event crosses as two table entries after the functions, in the description's order:
//   Subscribe<Name>(std::uint32_t scope, const ScriptCallback<Payload>* callback, std::uint64_t* outSubscription)
//   Unsubscribe<Name>(std::uint64_t subscription)
// The scope is an entity; an invalid one is refused with GE_Result_InvalidArg. The callback's
// Invoke runs on the event's delivery thread, and its Ended once, when the subscription ends.
//
// The C# surface: a function whose first parameter is an Entity is a member of the module's
// view of an entity (`ball.Physics.AddImpulse(v)`), not a static method; an event is an event of
// the view (`ball.Physics.ContactBegin += OnContact`), whose handler is `<Payload>Handler(in Payload)`.
// A Get<X> and Set<X> pair stays two methods: the view is a value an expression returns, and C#
// refuses to assign a property of it (CS1612).
namespace BindingRules
{

// The declarations a binding names, by id ("Input.ActionState"), over every module.
struct Index
{
    std::map<std::string, const ApiRecord*> Records;
    std::map<std::string, const ApiEnum*> Enums;
    std::map<std::string, const ApiConstant*> Constants;
};

Index BuildIndex(const ApiDescription& description);

// Every declaration of the description the generators cannot emit, named with its header,
// line and the fix: a kind outside the crossing above, an event whose scope is not an entity, or
// an event with a context.
std::vector<ApiRefusal> Unsupported(const ApiDescription& description);

// ---- The native binding ----

// "Input.script.gen": the stem of the generated header and translation unit.
std::string NativeFileStem(const ApiModule& module);
// "GE_Input_Table_v1".
std::string NativeTableType(const ApiModule& module);
// "GE_Input_GetInterface".
std::string NativeGetInterface(const ApiModule& module);
// "GE_Input_Hash".
std::string NativeHashConstant(const ApiModule& module);
// "IsKeyDownThunk"; for an event entry, "SubscribeContactBeginThunk".
std::string NativeThunk(const ApiFunction& function);
std::string NativeThunk(std::string_view name);
// "SubscribeContactBegin" and "UnsubscribeContactBegin": an event's table entries.
std::string SubscribeName(const ApiEvent& event);
std::string UnsubscribeName(const ApiEvent& event);
// "ge_input_is_key_down": the named export of the same body on the web.
std::string WebExport(const ApiModule& module, std::string_view name);
// "Input.IsKeyDown": how a refusal and the last error name the function, in every language.
std::string FunctionLabel(const ApiModule& module, std::string_view name);

// The C++ type of a value of `type` ("std::int32_t", "GameEngine::Input::ActionState").
std::string NativeValueType(const ApiType& type, const Index& index);
// True when an in value of `type` crosses as a pointer.
bool CrossesByPointer(const ApiType& type);
// The C++ type an in parameter crosses as ("std::uint8_t", "const GameEngine::Input::ActionBinding*").
std::string NativeInType(const ApiType& type, const Index& index);
// The C++ type an out parameter or the return value crosses as ("std::uint8_t*").
std::string NativeOutType(const ApiType& type, const Index& index);
// True when an out value of `type` is written through a local of the C++ type first (Bool, Entity).
bool OutConverts(const ApiType& type);
// The value an out parameter or the return value of `type` holds before the call: "{}", or the
// invalid entity for an Entity, so a call that does not run never names entity 0.
std::string NativeOutDefault(const ApiType& type);
// The expression that turns the C++ value `value` of `type` into its crossing ("value ? 1u : 0u", "value.id").
std::string NativeToCrossing(const ApiType& type, const std::string& value);
// The C++ type an event's callback crosses as ("const GameEngine::ScriptCallback<...::ContactEvent>*").
std::string NativeCallbackType(const ApiEvent& event, const Index& index);

// ---- The C# binding ----

// "GameEngine.Scripting".
std::string_view CSharpNamespace();
// "Input.g.cs".
std::string CSharpFile(const ApiModule& module);
// "Input": the static partial class of a module.
std::string CSharpModuleClass(const ApiModule& module);
// "IsKeyDown".
std::string CSharpMethod(const ApiFunction& function);
// "s_IsKeyDown": the static field that holds a table entry's pointer.
std::string CSharpPointerField(std::string_view name);
// A parameter's C# name: the description's name, a C# keyword escaped with @.
std::string CSharpParameter(std::string_view name);
// A record field's C# name: "justPressed" is "JustPressed".
std::string CSharpField(std::string_view name);
// A constant's C# name: "kAllLayers" is "AllLayers".
std::string CSharpConstant(std::string_view name);
// The C# type a caller sees ("bool", "Vector2", "StringId", "ActionState").
std::string CSharpPublicType(const ApiType& type, const Index& index);
// The C# type an in parameter crosses as ("byte", "ActionBinding*").
std::string CSharpInType(const ApiType& type, const Index& index);
// The C# type of the slot an out parameter or the return value is written to ("byte", "Vector2").
std::string CSharpSlotType(const ApiType& type, const Index& index);
// The C# initializer of such a slot: "default", or 0xFFFFFFFFu (Entity.None) for an Entity.
std::string CSharpSlotDefault(const ApiType& type);
// The C# literal of a parameter default, or of a constant ("1f", "DeviceType.Keyboard").
std::string CSharpLiteral(const ApiType& type, std::string_view literal, const Index& index);
// A reference to a constant of the description ("Kinds.AllLayers"), by its id ("Kinds.kAllLayers").
std::string CSharpConstantReference(const std::string& constantId, const Index& index);
// The C# initializer of a record field's default ("1f", "new Vector3(0f, 1f, 0f)"); empty for a
// field whose default is zero.
std::string CSharpFieldDefault(const ApiField& field, const Index& index);
// The sentence the C# documentation gives a function's thread rule.
std::string_view CSharpThreadRule(ApiThread thread);
// The sentence the C# documentation gives an event's delivery.
std::string_view CSharpDeliveryRule(ApiDelivery delivery);
// True when the function is a member of the module's entity view: its first parameter is an Entity in.
bool IsEntityMember(const ApiFunction& function);
// "View": the module class's nested view of an entity. "Physics": the Entity property that returns it.
std::string_view CSharpViewType();
std::string CSharpViewProperty(const ApiModule& module);
// "ContactEventHandler": the delegate an event with the payload `payloadId` calls.
std::string CSharpHandlerType(const std::string& payloadId, const Index& index);
// "OnContactBegin": the [UnmanagedCallersOnly] function the native side calls for an event.
std::string CSharpTrampoline(const ApiEvent& event);

} // namespace BindingRules

} // namespace GameEngine::ComponentScanner
