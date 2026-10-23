#include "BindingRules.h"

#include <cctype>

namespace GameEngine::ComponentScanner::BindingRules
{
namespace
{

constexpr std::string_view kUnsupportedFix =
    "use a kind of the crossing the generators emit (docs/scripting/exposing-a-function.mdx), or leave the "
    "declaration unmarked";

bool Emitted(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Void:
    case ApiKind::Bool:
    case ApiKind::Int8:
    case ApiKind::Int16:
    case ApiKind::Int32:
    case ApiKind::Int64:
    case ApiKind::UInt8:
    case ApiKind::UInt16:
    case ApiKind::UInt32:
    case ApiKind::UInt64:
    case ApiKind::Float:
    case ApiKind::Double:
    case ApiKind::Angle:
    case ApiKind::Vec2:
    case ApiKind::Vec3:
    case ApiKind::Vec4:
    case ApiKind::Quat:
    case ApiKind::Mat4:
    case ApiKind::StringId:
    case ApiKind::Record:
    case ApiKind::Enum:
    case ApiKind::Entity:
    case ApiKind::RenderLayerMask:
    case ApiKind::String:
    case ApiKind::ColorLinear:
        return true;
    case ApiKind::Span:
    case ApiKind::Handle:
        return false;
    }
    return false;
}

void RefuseKind(std::vector<ApiRefusal>& refusals, const ApiSource& source, const ApiType& type, std::string_view what)
{
    if (Emitted(type.Kind))
        return;
    refusals.push_back({source.File, source.Line,
                        "the binding generators do not emit the " + std::string(ApiKindName(type.Kind)) + " kind (" +
                            std::string(what) + ")",
                        std::string(kUnsupportedFix)});
}

// A span parameter: emitted when the function writes it (a span of results) and its element is a
// record or an entity, whose bytes the C# element type lays out the same.
void RefuseSpan(std::vector<ApiRefusal>& refusals, const ApiSource& source, const ApiParam& param)
{
    if (!param.Out)
    {
        RefuseKind(refusals, source, param.Type, "parameter `" + param.Name + "`");
        return;
    }
    const ApiKind element = param.Type.Element.front().Kind;
    if (element == ApiKind::Record || element == ApiKind::Entity)
        return;
    refusals.push_back({source.File, source.Line,
                        "the binding generators emit a span of results of records or entities only (parameter `" + param.Name +
                            "` holds " + std::string(ApiKindName(element)) + ")",
                        "write the results as a marked record, or as entities"});
}

std::string IntegerType(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Int8: return "std::int8_t";
    case ApiKind::Int16: return "std::int16_t";
    case ApiKind::Int32: return "std::int32_t";
    case ApiKind::Int64: return "std::int64_t";
    case ApiKind::UInt8: return "std::uint8_t";
    case ApiKind::UInt16: return "std::uint16_t";
    case ApiKind::UInt32: return "std::uint32_t";
    case ApiKind::UInt64: return "std::uint64_t";
    default: return {};
    }
}

std::string CSharpInteger(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Int8: return "sbyte";
    case ApiKind::Int16: return "short";
    case ApiKind::Int32: return "int";
    case ApiKind::Int64: return "long";
    case ApiKind::UInt8: return "byte";
    case ApiKind::UInt16: return "ushort";
    case ApiKind::UInt32: return "uint";
    case ApiKind::UInt64: return "ulong";
    default: return {};
    }
}

// The float-component value kinds (the Mathematics types and the linear color), which cross by
// pointer and take their defaults one component each.
std::string_view CSharpMath(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Vec2: return "Vector2";
    case ApiKind::Vec3: return "Vector3";
    case ApiKind::Vec4: return "Vector4";
    case ApiKind::Quat: return "Quaternion";
    case ApiKind::Mat4: return "Matrix4x4";
    case ApiKind::ColorLinear: return "ColorLinear";
    default: return {};
    }
}

std::string_view NativeMath(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Vec2: return "GameEngine::Mathematics::Vector2";
    case ApiKind::Vec3: return "GameEngine::Mathematics::Vector3";
    case ApiKind::Vec4: return "GameEngine::Mathematics::Vector4";
    case ApiKind::Quat: return "GameEngine::Mathematics::Quaternion";
    case ApiKind::Mat4: return "GameEngine::Mathematics::Matrix4x4";
    case ApiKind::ColorLinear: return "GameEngine::ColorLinear";
    default: return {};
    }
}

std::string SnakeCase(std::string_view name)
{
    std::string text;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isupper(c) != 0)
        {
            const bool afterLower = i > 0 && std::islower(static_cast<unsigned char>(name[i - 1])) != 0;
            const bool beforeLower = i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1])) != 0 &&
                                     i > 0 && std::isupper(static_cast<unsigned char>(name[i - 1])) != 0;
            if (afterLower || beforeLower)
                text += '_';
            text += static_cast<char>(std::tolower(c));
        }
        else
            text += static_cast<char>(c);
    }
    return text;
}

constexpr std::string_view kCSharpKeywords[] = {
    "abstract", "as",       "base",     "bool",      "break",     "byte",     "case",      "catch",    "char",
    "checked",  "class",    "const",    "continue",  "decimal",   "default",  "delegate",  "do",       "double",
    "else",     "enum",     "event",    "explicit",  "extern",    "false",    "finally",   "fixed",    "float",
    "for",      "foreach",  "goto",     "if",        "implicit",  "in",       "int",       "interface", "internal",
    "is",       "lock",     "long",     "namespace", "new",       "null",     "object",    "operator", "out",
    "override", "params",   "private",  "protected", "public",    "readonly", "ref",       "return",   "sbyte",
    "sealed",   "short",    "sizeof",   "stackalloc", "static",   "string",   "struct",    "switch",   "this",
    "throw",    "true",     "try",      "typeof",    "uint",      "ulong",    "unchecked", "unsafe",   "ushort",
    "using",    "virtual",  "void",     "volatile",  "while"};

std::string FloatLiteral(std::string_view literal)
{
    return std::string(literal) + "f";
}

} // namespace

Index BuildIndex(const ApiDescription& description)
{
    Index index;
    for (const ApiModule& module : description.Modules)
    {
        for (const ApiRecord& record : module.Records)
            index.Records[module.Name + "." + record.Name] = &record;
        for (const ApiEnum& e : module.Enums)
            index.Enums[module.Name + "." + e.Name] = &e;
        for (const ApiConstant& constant : module.Constants)
            index.Constants[module.Name + "." + constant.Name] = &constant;
    }
    return index;
}

std::vector<ApiRefusal> Unsupported(const ApiDescription& description)
{
    std::vector<ApiRefusal> refusals;
    for (const ApiModule& module : description.Modules)
    {
        for (const ApiFunction& function : module.Functions)
        {
            RefuseKind(refusals, function.Source, function.Returns, "the return type of `" + function.Name + "`");
            for (const ApiParam& param : function.Params)
            {
                if (param.Type.Kind == ApiKind::Span)
                    RefuseSpan(refusals, function.Source, param);
                else
                    RefuseKind(refusals, function.Source, param.Type, "parameter `" + param.Name + "`");
            }
        }
        for (const ApiRecord& record : module.Records)
            for (const ApiField& field : record.Fields)
            {
                RefuseKind(refusals, record.Source, field.Type, "field `" + field.Name + "` of `" + record.Name + "`");
                if (field.Type.Kind == ApiKind::RenderLayerMask)
                    refusals.push_back({record.Source.File, record.Source.Line,
                                        "the binding generators emit a RenderLayerMask as a parameter or a return value only "
                                        "(field `" + field.Name + "` of `" + record.Name + "`)",
                                        "hold the mask's bits as a std::uint32_t field"});
            }
        for (const ApiEvent& event : module.Events)
        {
            if (event.Scope.Kind != ApiKind::Entity)
                refusals.push_back({event.Source.File, event.Source.Line,
                                    "the binding generators emit events scoped to an entity only (`" + event.Name +
                                        "` is scoped to a " + std::string(ApiKindName(event.Scope.Kind)) + ")",
                                    std::string(kUnsupportedFix)});
            if (event.Context)
                refusals.push_back({event.Source.File, event.Source.Line,
                                    "the binding generators emit events with no @ge-context (`" + event.Name + "`)",
                                    "reach the system from the facade body, or leave the event unmarked"});
        }
    }
    return refusals;
}

std::string NativeFileStem(const ApiModule& module)
{
    return module.Name + ".script.gen";
}

std::string NativeTableType(const ApiModule& module)
{
    return "GE_" + module.Name + "_Table_v" + std::to_string(kApiDescriptionFormat);
}

std::string NativeGetInterface(const ApiModule& module)
{
    return "GE_" + module.Name + "_GetInterface";
}

std::string NativeHashConstant(const ApiModule& module)
{
    return "GE_" + module.Name + "_Hash";
}

std::string NativeThunk(const ApiFunction& function)
{
    return NativeThunk(function.Name);
}

std::string NativeThunk(std::string_view name)
{
    return std::string(name) + "Thunk";
}

std::string SubscribeName(const ApiEvent& event)
{
    return "Subscribe" + event.Name;
}

std::string UnsubscribeName(const ApiEvent& event)
{
    return "Unsubscribe" + event.Name;
}

std::string WebExport(const ApiModule& module, std::string_view name)
{
    return "ge_" + SnakeCase(module.Name) + "_" + SnakeCase(name);
}

std::string FunctionLabel(const ApiModule& module, std::string_view name)
{
    return module.Name + "." + std::string(name);
}

std::vector<NativeParameter> NativeParameters(const ApiParam& param, const Index& index)
{
    if (param.Type.Kind == ApiKind::String)
        return {{"const char*", param.Name}, {"std::int32_t", TextLength(param.Name)}};
    if (param.Type.Kind == ApiKind::Span)
        return {{NativeValueType(param.Type.Element.front(), index) + "*", param.Name}, {"std::int32_t", Capacity(param.Name)}};
    return {{param.Out ? NativeOutType(param.Type, index) : NativeInType(param.Type, index), param.Name}};
}

std::vector<NativeParameter> NativeResultParameters(const ApiType& returns, const Index& index)
{
    if (returns.Kind == ApiKind::Void)
        return {};
    if (returns.Kind == ApiKind::String)
        return {{"char*", "outResult"}, {"std::int32_t", Capacity("outResult")}, {"std::int32_t*", TextLength("outResult")}};
    return {{NativeOutType(returns, index), "outResult"}};
}

std::string TextLength(std::string_view name)
{
    return std::string(name) + "Length";
}

std::string Capacity(std::string_view name)
{
    return std::string(name) + "Capacity";
}

bool IsSpanOut(const ApiParam& param)
{
    return param.Out && param.Type.Kind == ApiKind::Span;
}

bool HasText(const ApiFunction& function)
{
    if (function.Returns.Kind == ApiKind::String)
        return true;
    for (const ApiParam& param : function.Params)
        if (param.Type.Kind == ApiKind::String)
            return true;
    return false;
}

std::string NativeValueType(const ApiType& type, const Index& index)
{
    switch (type.Kind)
    {
    case ApiKind::Void: return "void";
    case ApiKind::Bool: return "bool";
    case ApiKind::Float:
    case ApiKind::Angle: return "float";
    case ApiKind::Double: return "double";
    case ApiKind::StringId: return "std::uint64_t";
    case ApiKind::Entity: return "GameEngine::ECS::EntityHandle";
    case ApiKind::RenderLayerMask: return "GameEngine::Rendering::RenderLayerMask";
    case ApiKind::Record: return index.Records.at(type.TypeId)->CppName;
    case ApiKind::Enum: return index.Enums.at(type.TypeId)->CppName;
    default: break;
    }
    if (const std::string integer = IntegerType(type.Kind); !integer.empty())
        return integer;
    return std::string(NativeMath(type.Kind));
}

bool CrossesByPointer(const ApiType& type)
{
    return type.Kind == ApiKind::Record || !NativeMath(type.Kind).empty();
}

std::string NativeInType(const ApiType& type, const Index& index)
{
    if (type.Kind == ApiKind::Bool)
        return "std::uint8_t";
    if (type.Kind == ApiKind::Entity || type.Kind == ApiKind::RenderLayerMask)
        return "std::uint32_t";
    if (CrossesByPointer(type))
        return "const " + NativeValueType(type, index) + "*";
    return NativeValueType(type, index);
}

std::string NativeOutType(const ApiType& type, const Index& index)
{
    if (type.Kind == ApiKind::Bool)
        return "std::uint8_t*";
    if (type.Kind == ApiKind::Entity || type.Kind == ApiKind::RenderLayerMask)
        return "std::uint32_t*";
    return NativeValueType(type, index) + "*";
}

std::string NativeOutDefault(const ApiType& type)
{
    return type.Kind == ApiKind::Entity ? "::GameEngine::ECS::kInvalidEntity" : "{}";
}

bool OutConverts(const ApiType& type)
{
    return type.Kind == ApiKind::Bool || type.Kind == ApiKind::Entity || type.Kind == ApiKind::RenderLayerMask;
}

std::string NativeToCrossing(const ApiType& type, const std::string& value)
{
    if (type.Kind == ApiKind::Bool)
        return value + " ? 1u : 0u";
    if (type.Kind == ApiKind::Entity)
        return value + ".id";
    if (type.Kind == ApiKind::RenderLayerMask)
        return value + ".Bits";
    return value;
}

std::string NativeCallbackType(const ApiEvent& event, const Index& index)
{
    return "const GameEngine::ScriptCallback<" + index.Records.at(event.PayloadId)->CppName + ">*";
}

std::string_view CSharpNamespace()
{
    return "GameEngine.Scripting";
}

std::string CSharpFile(const ApiModule& module)
{
    return module.Name + ".g.cs";
}

std::string CSharpModuleClass(const ApiModule& module)
{
    return module.Name;
}

std::string CSharpMethod(const ApiFunction& function)
{
    return function.Name;
}

std::string CSharpPointerField(std::string_view name)
{
    return "s_" + std::string(name);
}

std::string CSharpParameter(std::string_view name)
{
    for (std::string_view keyword : kCSharpKeywords)
        if (keyword == name)
            return "@" + std::string(name);
    return std::string(name);
}

std::string CSharpField(std::string_view name)
{
    std::string text(name);
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

std::string CSharpConstant(std::string_view name)
{
    if (name.size() > 1 && name[0] == 'k' && std::isupper(static_cast<unsigned char>(name[1])) != 0)
        return std::string(name.substr(1));
    return CSharpField(name);
}

std::string CSharpPublicType(const ApiType& type, const Index& index)
{
    switch (type.Kind)
    {
    case ApiKind::Void: return "void";
    case ApiKind::Bool: return "bool";
    case ApiKind::Float:
    case ApiKind::Angle: return "float";
    case ApiKind::Double: return "double";
    case ApiKind::StringId: return "StringId";
    case ApiKind::Entity: return "Entity";
    case ApiKind::RenderLayerMask: return "RenderLayerMask";
    case ApiKind::String: return "string";
    case ApiKind::Span: return "Span<" + CSharpPublicType(type.Element.front(), index) + ">";
    case ApiKind::Record: return index.Records.at(type.TypeId)->Name;
    case ApiKind::Enum: return index.Enums.at(type.TypeId)->Name;
    default: break;
    }
    if (const std::string integer = CSharpInteger(type.Kind); !integer.empty())
        return integer;
    return std::string(CSharpMath(type.Kind));
}

std::string CSharpInType(const ApiType& type, const Index& index)
{
    if (type.Kind == ApiKind::Bool)
        return "byte";
    if (type.Kind == ApiKind::StringId)
        return "ulong";
    if (type.Kind == ApiKind::Entity || type.Kind == ApiKind::RenderLayerMask)
        return "uint";
    if (type.Kind == ApiKind::String)
        return "byte*, int";
    if (type.Kind == ApiKind::Span)
        return CSharpPublicType(type.Element.front(), index) + "*, int";
    if (CrossesByPointer(type))
        return CSharpPublicType(type, index) + "*";
    return CSharpPublicType(type, index);
}

std::string_view CSharpTextResultTypes()
{
    return "byte*, int, int*";
}

std::string_view CSharpTextHelper()
{
    return "ScriptText";
}

std::string CSharpSlotDefault(const ApiType& type)
{
    return type.Kind == ApiKind::Entity ? "0xFFFFFFFFu" : "default";
}

std::string CSharpSlotType(const ApiType& type, const Index& index)
{
    if (type.Kind == ApiKind::Bool)
        return "byte";
    if (type.Kind == ApiKind::StringId)
        return "ulong";
    if (type.Kind == ApiKind::Entity || type.Kind == ApiKind::RenderLayerMask)
        return "uint";
    return CSharpPublicType(type, index);
}

std::string CSharpLiteral(const ApiType& type, std::string_view literal, const Index& index)
{
    switch (type.Kind)
    {
    case ApiKind::Float:
    case ApiKind::Angle: return FloatLiteral(literal);
    case ApiKind::Double: return std::string(literal) + "d";
    case ApiKind::Enum: return index.Enums.at(type.TypeId)->Name + "." + std::string(literal);
    case ApiKind::UInt32: return std::string(literal) + "u";
    case ApiKind::Int64: return std::string(literal) + "L";
    case ApiKind::UInt64: return std::string(literal) + "UL";
    default: return std::string(literal);
    }
}

std::string CSharpConstantReference(const std::string& constantId, const Index& index)
{
    const std::size_t dot = constantId.find('.');
    return constantId.substr(0, dot) + "." + CSharpConstant(index.Constants.at(constantId)->Name);
}

std::string CSharpFieldDefault(const ApiField& field, const Index& index)
{
    if (field.DefaultConstantId)
        return CSharpConstantReference(*field.DefaultConstantId, index);
    if (field.Type.Kind == ApiKind::Record)
        return "new " + CSharpPublicType(field.Type, index) + "()";
    // EntityHandle{} is the invalid entity; C# default(Entity) would be the valid id 0.
    if (field.Type.Kind == ApiKind::Entity)
        return "Entity.None";
    if (field.Default.empty())
        return {};
    if (!CSharpMath(field.Type.Kind).empty())
    {
        bool zero = true;
        std::string arguments;
        for (const std::string& component : field.Default)
        {
            zero = zero && component == "0";
            arguments += (arguments.empty() ? "" : ", ") + FloatLiteral(component);
        }
        if (zero)
            return {};
        return "new " + std::string(CSharpMath(field.Type.Kind)) + "(" + arguments + ")";
    }
    const std::string& literal = field.Default.front();
    if (literal == "0" || literal == "false")
        return {};
    if (field.Type.Kind == ApiKind::Enum)
    {
        const ApiEnum& e = *index.Enums.at(field.Type.TypeId);
        for (const ApiEnumValue& value : e.Values)
            if (value.Name == literal && value.Value == 0)
                return {};
    }
    return CSharpLiteral(field.Type, literal, index);
}

std::string_view CSharpDeliveryRule(ApiDelivery delivery)
{
    switch (delivery)
    {
    case ApiDelivery::PhysicsStep:
        return "The handler runs on the main thread inside the physics step, after the bodies moved. Subscribe and "
               "unsubscribe on the main thread, in OnEnable and OnDisable. The subscription ends when you unsubscribe, "
               "when your script unloads (a hot reload), or when the physics world shuts down (leaving play mode).";
    case ApiDelivery::UiEventRouting:
        return "The handler runs on the main thread while the UI routes the event. Subscribe and unsubscribe on the "
               "main thread.";
    }
    return {};
}

bool IsEntityMember(const ApiFunction& function)
{
    return !function.Params.empty() && function.Params.front().Type.Kind == ApiKind::Entity && !function.Params.front().Out;
}

std::string_view CSharpViewType()
{
    return "View";
}

std::string CSharpViewProperty(const ApiModule& module)
{
    return module.Name;
}

std::string CSharpHandlerType(const std::string& payloadId, const Index& index)
{
    return index.Records.at(payloadId)->Name + "Handler";
}

std::string CSharpTrampoline(const ApiEvent& event)
{
    return "On" + event.Name;
}

std::string_view CSharpThreadRule(ApiThread thread)
{
    switch (thread)
    {
    case ApiThread::Main:
        return "Call it on the main thread. A call from any other thread, or from a job, throws EngineCallException.";
    case ApiThread::Engine:
        return "Call it on the main thread or from a system the engine runs. A call from a thread the engine does not "
               "run (Task.Run, a timer) throws EngineCallException.";
    case ApiThread::Any:
        return "Any thread may call it.";
    }
    return {};
}

} // namespace GameEngine::ComponentScanner::BindingRules
