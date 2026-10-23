#pragma once

#include "ApiModel.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The C# mirrors of the engine's components: one generated `partial struct` per component, at
// the C++ offsets, in GameEngine.Scripting (assembly GameEngine.Scripting.Bindings), and one native
// unit that pins every size, alignment, offset and enum value to the compiler's. The rules:
//
// Classes. Every reflected component is one of:
//   editor-only       // @ge-editor-only on the struct   no mirror
//   engine-internal   // @ge-engine-internal              no mirror: engine runtime state is no game API
//   engine-owned      // @ge-readonly on the struct       a `readonly partial struct`: every member get-only
//   game data         no marker                           a full mirror
// A reflected struct that a mirrored component holds as a field is an element struct: a mirror with
// no IComponent and no [BuiltInComponent].
//
// Fields. Each field takes one kind; a field no kind covers refuses the component:
//   a scalar, a Mathematics vector, quaternion or matrix, ColorLinear, ECS::EntityHandle, StringId,
//   Rendering::RenderLayerMask       the C# type of the description's kind (BindingRules.h)
//   AssetRef<C> and its aliases, GUID   AssetGuid, 16 bytes in the engine's text order
//   Math::Curve                      Curve (644 bytes);  Mathematics::BoundingBox   BoundingBox
//   an enum                          a property over a private field, whose setter refuses a value
//                                    outside the enum with ArgumentOutOfRangeException
//   char name[N]                     text: a string property over N private bytes, UTF-8 up to the
//                                    first zero; the setter refuses more than N - 1 bytes
//   T name[N]                        a field of the nested [InlineArray(N)] struct <Field>Array
//   a reflected struct               its mirror
// GUID, Math::Curve and Mathematics::BoundingBox take their kind only when the written name names
// the engine type from the component's scope, as C++ looks it up. A field marked // @ge-hidden is
// a private member; one marked // @ge-readonly is get-only.
//
// Arrays. The element is any kind but an enum and text. The extent is an integer literal, or + and
// - over integer literals and constexpr integer constants declared in the same struct, the same
// header or another scanned component header; a constant defined by other constants is followed.
// An array field holds at most 2^31 - 1 bytes, the most a C# struct holds.
//
// Enums. The mirror's enum table holds the enums of the component headers and of the Include trees
// of the modules the component headers include (`#include "Rendering/..."` reads
// Modules/Rendering/Include). A field's enum is the one its written name names from the
// component's scope, as C++ looks it up. Each enum is emitted once, in the file of the header that
// declares it, with its `///` comment as its summary; an enum a *ScriptApi.h marks is that
// module's C# enum.
//
// Defaults. A mirror holds no initializers: its parameterless constructor, a component's and an
// element struct's alike, copies the engine's default bytes (ComponentType<T>.Default); default(T)
// is all zeros.
//
// Refusals, each with the header, the line and the fix: a field no kind covers; an array of enums;
// an extent that is not a sum of literals and constants; an array field past 2^31 - 1 bytes; a
// nested array type another member names;
// a member named like its struct; an element struct with no mirror; an enum the table does not
// find, or finds twice; an enum with no explicit underlying type; a C# type name another generated
// type or a hand-written type beside the mirrors has.

// Where a script API scan with component roots writes the mirrors: the classes beside the
// description, the C# under the C# binding's directory, the pins beside the native bindings.
inline constexpr std::string_view kComponentClassesFile = "component-classes.txt";
inline constexpr std::string_view kComponentCSharpDir = "Components";
inline constexpr std::string_view kComponentLayoutFile = "Components.layout.gen.cpp";

enum class ComponentClass : std::uint8_t
{
    GameData,
    EngineOwned,
    EngineInternal,
    EditorOnly,
    Element,
};

// True for the classes that have a mirror.
bool HasMirror(ComponentClass componentClass);

enum class MirrorKind : std::uint8_t
{
    Value, // a kind of the description (ApiKind): a scalar, a Mathematics type, a color, an entity, ...
    AssetGuid,
    Curve,
    BoundingBox,
    Text,
    Enum,
    Struct,
};

struct MirrorField
{
    std::string Name; // the C++ member name
    MirrorKind Kind = MirrorKind::Value;
    ApiKind Value = ApiKind::Void; // the kind of a Value field
    std::string TypeName;          // the qualified C++ name of an Enum or a Struct field
    std::uint32_t Extent = 0;      // the elements of an array, the bytes of text; 0 for one value
    std::uint32_t Offset = 0;
    std::uint32_t Size = 0; // the whole field, in bytes
    bool Hidden = false;
    bool ReadOnly = false;
    int Line = 0;
};

struct MirrorStruct
{
    std::string Name;
    std::string CppName;
    std::string Header; // the include path ("Components/Rendering/MeshRenderer.h")
    std::string File;   // the path the refusals print
    int Line = 0;
    std::string Doc;
    ComponentClass Class = ComponentClass::GameData;
    std::uint32_t Size = 0;
    std::uint32_t Align = 1;
    std::vector<MirrorField> Fields; // empty for a class with no mirror
};

struct MirrorEnum
{
    std::string Name;
    std::string CppName;
    std::string Header; // the include path of the header that declares it
    std::string Doc;
    ApiKind Underlying = ApiKind::Int32;
    std::vector<ApiEnumValue> Values;
    // The C# enum of a module's description: its module's binding emits and pins it.
    bool Bound = false;
};

// Every reflected component with its class, and the enums the mirrored fields use.
struct ComponentMirrors
{
    std::vector<MirrorStruct> Structs;
    std::vector<MirrorEnum> Enums;
};

// The class as component-classes.txt writes it ("game data, a full mirror").
std::string_view ComponentClassText(ComponentClass componentClass);

// component-classes.txt: one line per reflected component, sorted by name: its name, its class
// and the marker that decides it. A component that changes class is a one-line diff in review.
std::string ComponentClassesText(const ComponentMirrors& mirrors);

// The C# file of a header, relative to Generated/Components: "Components/Rendering/MeshRenderer.h"
// is "Rendering/MeshRenderer.g.cs", a module header "Rendering/ShadowMode.h" "Rendering/ShadowMode.g.cs".
std::string MirrorCSharpFile(std::string_view header);

// The C# name of a field's member: the C++ name with its first letter upper case.
std::string MirrorMemberName(std::string_view fieldName);

// The nested [InlineArray] type of an array or text field: "<Member>Array".
std::string MirrorArrayType(std::string_view fieldName);

} // namespace GameEngine::ComponentScanner
