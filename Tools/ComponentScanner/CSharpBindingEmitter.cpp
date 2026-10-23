#include "CSharpBindingEmitter.h"

#include "ApiJsonEmitter.h"

#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace GameEngine::ComponentScanner::CSharpBindingEmitter
{
namespace
{

using namespace BindingRules;

std::string HexHash(std::uint64_t hash)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llXUL", static_cast<unsigned long long>(hash));
    return text;
}

std::string XmlEscaped(std::string_view text)
{
    std::string escaped;
    for (const char c : text)
    {
        if (c == '&')
            escaped += "&amp;";
        else if (c == '<')
            escaped += "&lt;";
        else if (c == '>')
            escaped += "&gt;";
        else
            escaped += c;
    }
    return escaped;
}

} // namespace

void EmitSummary(std::string& out, std::string_view doc, std::string_view indent)
{
    out += std::string(indent) + "/// <summary>\n";
    std::size_t start = 0;
    while (start <= doc.size())
    {
        std::size_t end = doc.find('\n', start);
        if (end == std::string_view::npos)
            end = doc.size();
        const std::string_view line = doc.substr(start, end - start);
        out += std::string(indent) + "///" + (line.empty() ? "" : " " + XmlEscaped(line)) + "\n";
        start = end + 1;
    }
    out += std::string(indent) + "/// </summary>\n";
}

namespace
{

std::string CSharpEnumUnderlying(ApiKind kind, const Index& index)
{
    return CSharpPublicType(ApiType{kind, {}, {}}, index);
}

void EmitEnum(std::string& out, const ApiEnum& e, const Index& index)
{
    EmitSummary(out, e.Doc, "    ");
    if (e.Flags)
        out += "    [System.Flags]\n";
    out += "    public enum " + e.Name + " : " + CSharpEnumUnderlying(e.Underlying, index) + "\n    {\n";
    for (const ApiEnumValue& value : e.Values)
        out += "        " + value.Name + " = " + std::to_string(value.Value) + ",\n";
    out += "    }\n\n";
}

void EmitRecord(std::string& out, const ApiRecord& record, const Index& index)
{
    EmitSummary(out, record.Doc, "    ");
    out += "    [StructLayout(LayoutKind.Explicit, Size = " + std::to_string(record.Size) + ")]\n";
    out += "    public struct " + record.Name + "\n    {\n";
    out += "        /// <summary>\n        /// Creates the record with the default of every field.\n        /// </summary>\n";
    out += "        public " + record.Name + "()\n        {\n        }\n\n";
    for (const ApiField& field : record.Fields)
    {
        out += "        [FieldOffset(" + std::to_string(field.Offset) + ")]\n        public " +
               CSharpPublicType(field.Type, index) + " " + CSharpField(field.Name);
        const std::string initializer = CSharpFieldDefault(field, index);
        if (!initializer.empty())
            out += " = " + initializer;
        out += ";\n";
    }
    out += "    }\n\n";
}

bool IsRecordDefault(const ApiParam& param)
{
    return param.Type.Kind == ApiKind::Record && param.Default && param.Default->Literal == "{}";
}

// The local that holds the value a pointer argument points at, or that an out value is written to.
std::string Local(const ApiParam& param)
{
    return param.Name + "Value";
}

std::string PointerType(const ApiFunction& function, const Index& index)
{
    std::string text = "delegate* unmanaged[Cdecl]<";
    for (const ApiParam& param : function.Params)
        text += (param.Out && !IsSpanOut(param) ? CSharpSlotType(param.Type, index) + "*" : CSharpInType(param.Type, index)) + ", ";
    if (function.Returns.Kind == ApiKind::String)
        text += std::string(CSharpTextResultTypes()) + ", ";
    else if (function.Returns.Kind != ApiKind::Void)
        text += CSharpSlotType(function.Returns, index) + "*, ";
    return text + "int>";
}

constexpr std::string_view kSubscribePointer = "delegate* unmanaged[Cdecl]<uint, ScriptCallbackData*, ulong*, int>";
constexpr std::string_view kUnsubscribePointer = "delegate* unmanaged[Cdecl]<ulong, int>";

// The parameters a C# method declares: every parameter of the function, less the first one of an
// entity member, which is the view's entity.
std::vector<const ApiParam*> DeclaredParams(const ApiFunction& function, bool member)
{
    std::vector<const ApiParam*> params;
    for (std::size_t i = member ? 1 : 0; i < function.Params.size(); ++i)
        params.push_back(&function.Params[i]);
    return params;
}

std::string Signature(const ApiFunction& function, const Index& index, bool member)
{
    std::string text;
    for (const ApiParam* param : DeclaredParams(function, member))
    {
        if (!text.empty())
            text += ", ";
        if (param->Out && !IsSpanOut(*param))
            text += "out ";
        if (IsRecordDefault(*param))
        {
            text += CSharpPublicType(param->Type, index) + "? " + CSharpParameter(param->Name) + " = null";
            continue;
        }
        text += CSharpPublicType(param->Type, index) + " " + CSharpParameter(param->Name);
        if (param->Default)
            text += " = " + (param->Default->ConstantId ? CSharpConstantReference(*param->Default->ConstantId, index)
                                                         : CSharpLiteral(param->Type, param->Default->Literal, index));
    }
    return text;
}

// The C# expression that names a parameter's value in a body: its name, or what replaces it (the
// view's entity).
using Names = std::map<std::string, std::string>;

std::string NameOf(const ApiParam& param, const Names& names)
{
    const auto it = names.find(param.Name);
    return it != names.end() ? it->second : CSharpParameter(param.Name);
}

// The pointer a span of results passes for the call ("hitsPointer").
std::string SpanPointer(const std::string& name)
{
    return name + "Pointer";
}

// The fixed statement that pins a span of results for the call.
std::string SpanFixed(const ApiParam& param, const Index& index, const Names& names)
{
    return "fixed (" + CSharpPublicType(param.Type.Element.front(), index) + "* " + SpanPointer(param.Name) + " = " +
           NameOf(param, names) + ")";
}

std::string ArgumentFor(const ApiParam& param, const Names& names)
{
    const std::string name = NameOf(param, names);
    if (IsSpanOut(param))
        return SpanPointer(param.Name) + ", " + name + ".Length";
    if (param.Out || IsRecordDefault(param))
        return "&" + Local(param);
    switch (param.Type.Kind)
    {
    case ApiKind::Bool: return name + " ? (byte)1 : (byte)0";
    case ApiKind::StringId: return name + ".Value";
    case ApiKind::Entity: return name + ".Id";
    case ApiKind::RenderLayerMask: return name + ".Bits";
    default: break;
    }
    if (CrossesByPointer(param.Type))
        return "&" + name;
    return name;
}

// The C# value of a slot of `type` named `slot` ("result != 0", "new Entity(result)").
std::string FromSlot(const ApiType& type, const std::string& slot)
{
    switch (type.Kind)
    {
    case ApiKind::Bool: return slot + " != 0";
    case ApiKind::StringId: return "new StringId(" + slot + ")";
    case ApiKind::Entity: return "new Entity(" + slot + ")";
    case ApiKind::RenderLayerMask: return "new RenderLayerMask(" + slot + ")";
    default: return slot;
    }
}

// The local that holds a text's bytes for the call ("nameUtf8"), and the pointer the call passes ("namePointer").
std::string TextLocal(const std::string& name)
{
    return name + "Utf8";
}

std::string TextPointer(const std::string& name)
{
    return name + "Pointer";
}

// The statements of a call that takes or returns text: each text argument encoded as UTF-8 for the
// call, the call inside the fixed statements that pin the bytes, and for a text return a second call
// with a buffer of the length the engine reported when the text did not fit. The pooled buffers go
// back in the finally block.
void EmitTextBody(std::string& out, const ApiModule& module, const ApiFunction& function, const Index& index,
                  std::string_view indent, const Names& names)
{
    const std::string pad(indent);
    const std::string helper(CSharpTextHelper());
    const bool textResult = function.Returns.Kind == ApiKind::String;
    std::vector<std::string> texts;
    for (const ApiParam& param : function.Params)
    {
        if (IsRecordDefault(param))
            out += pad + CSharpPublicType(param.Type, index) + " " + Local(param) + " = " + NameOf(param, names) + " ?? new " +
                   CSharpPublicType(param.Type, index) + "();\n";
        if (param.Type.Kind != ApiKind::String)
            continue;
        out += pad + "ArgumentNullException.ThrowIfNull(" + NameOf(param, names) + ");\n";
        out += pad + helper + ".In " + TextLocal(param.Name) + " = new " + helper + ".In(" + NameOf(param, names) +
               ", stackalloc byte[" + helper + ".StackBytes]);\n";
        texts.push_back(param.Name);
    }
    if (textResult)
    {
        out += pad + helper + ".Out " + TextLocal("result") + " = new " + helper + ".Out(stackalloc byte[" + helper +
               ".StackBytes]);\n";
        texts.push_back("result");
    }
    out += pad + "try\n" + pad + "{\n";
    std::string inner = pad + "    ";
    if (textResult)
    {
        out += inner + "while (true)\n" + inner + "{\n";
        inner += "    ";
    }
    std::string arguments;
    for (const ApiParam& param : function.Params)
    {
        if (param.Out && !IsSpanOut(param))
            out += inner + CSharpSlotType(param.Type, index) + " " + Local(param) + " = " + CSharpSlotDefault(param.Type) + ";\n";
        const std::string argument = param.Type.Kind == ApiKind::String
                                         ? TextPointer(param.Name) + ", " + TextLocal(param.Name) + ".Bytes.Length"
                                         : ArgumentFor(param, names);
        arguments += (arguments.empty() ? "" : ", ") + argument;
    }
    if (textResult)
    {
        out += inner + "int resultLength = 0;\n";
        arguments += std::string(arguments.empty() ? "" : ", ") + TextPointer("result") + ", " + TextLocal("result") +
                     ".Bytes.Length, &resultLength";
    }
    else if (function.Returns.Kind != ApiKind::Void)
    {
        out += inner + CSharpSlotType(function.Returns, index) + " result = " + CSharpSlotDefault(function.Returns) + ";\n";
        arguments += std::string(arguments.empty() ? "" : ", ") + "&result";
    }
    out += inner + "int code;\n";
    for (const std::string& text : texts)
        out += inner + "fixed (byte* " + TextPointer(text) + " = " + TextLocal(text) + ".Bytes)\n";
    for (const ApiParam& param : function.Params)
        if (IsSpanOut(param))
            out += inner + SpanFixed(param, index, names) + "\n";
    out += inner + "    code = " + CSharpPointerField(function.Name) + "(" + arguments + ");\n";
    out += inner + "if (code != 0 && code != BindingErrors.NotInitialized)\n" + inner + "    BindingErrors.Throw(code, \"" +
           FunctionLabel(module, function.Name) + "\");\n";
    if (textResult)
        out += inner + "if (!" + TextLocal("result") + ".TryRead(resultLength, out string result))\n" + inner + "    continue;\n";
    for (const ApiParam& param : function.Params)
        if (param.Out && !IsSpanOut(param))
            out += inner + NameOf(param, names) + " = " + FromSlot(param.Type, Local(param)) + ";\n";
    if (function.Returns.Kind != ApiKind::Void)
        out += inner + "return " + FromSlot(function.Returns, "result") + ";\n";
    if (textResult)
        out += pad + "    }\n";
    out += pad + "}\n" + pad + "finally\n" + pad + "{\n";
    for (const std::string& text : texts)
        out += pad + "    " + TextLocal(text) + ".Dispose();\n";
    out += pad + "}\n";
}

// The statements of a call: the locals, the call through the table (inside the fixed statements that pin
// each span of results), the error mapping, the out values and the return.
void EmitBody(std::string& out, const ApiModule& module, const ApiFunction& function, const Index& index, std::string_view indent,
              const Names& names)
{
    if (HasText(function))
    {
        EmitTextBody(out, module, function, index, indent, names);
        return;
    }
    const std::string pad(indent);
    std::string arguments;
    for (const ApiParam& param : function.Params)
    {
        if (IsRecordDefault(param))
            out += pad + CSharpPublicType(param.Type, index) + " " + Local(param) + " = " + NameOf(param, names) + " ?? new " +
                   CSharpPublicType(param.Type, index) + "();\n";
        else if (param.Out && !IsSpanOut(param))
            out += pad + CSharpSlotType(param.Type, index) + " " + Local(param) + " = " + CSharpSlotDefault(param.Type) + ";\n";
        arguments += (arguments.empty() ? "" : ", ") + ArgumentFor(param, names);
    }
    const bool hasResult = function.Returns.Kind != ApiKind::Void;
    if (hasResult)
    {
        out += pad + CSharpSlotType(function.Returns, index) + " result = " + CSharpSlotDefault(function.Returns) + ";\n";
        arguments += std::string(arguments.empty() ? "" : ", ") + "&result";
    }
    std::string pins;
    for (const ApiParam& param : function.Params)
        if (IsSpanOut(param))
            pins += pad + SpanFixed(param, index, names) + "\n";
    if (pins.empty())
        out += pad + "int code = " + CSharpPointerField(function.Name) + "(" + arguments + ");\n";
    else
        out += pad + "int code;\n" + pins + pad + "    code = " + CSharpPointerField(function.Name) + "(" + arguments + ");\n";
    out += pad + "if (code != 0 && code != BindingErrors.NotInitialized)\n" + pad + "    BindingErrors.Throw(code, \"" +
           FunctionLabel(module, function.Name) + "\");\n";
    for (const ApiParam& param : function.Params)
        if (param.Out && !IsSpanOut(param))
            out += pad + NameOf(param, names) + " = " + FromSlot(param.Type, Local(param)) + ";\n";
    if (hasResult)
        out += pad + "return " + FromSlot(function.Returns, "result") + ";\n";
}

void EmitMethod(std::string& out, const ApiModule& module, const ApiFunction& function, const Index& index, bool member)
{
    const std::string_view indent = member ? "            " : "        ";
    EmitSummary(out, function.Doc, indent);
    out += std::string(indent) + "/// <remarks>" + std::string(CSharpThreadRule(function.Thread)) + "</remarks>\n";
    out += std::string(indent) + "public " + (member ? "" : "static ") + CSharpPublicType(function.Returns, index) + " " +
           CSharpMethod(function) + "(" + Signature(function, index, member) + ")\n" + std::string(indent) + "{\n";
    Names names;
    if (member)
        names[function.Params.front().Name] = "m_Entity";
    EmitBody(out, module, function, index, std::string(indent) + "    ", names);
    out += std::string(indent) + "}\n";
}

void EmitViewEvent(std::string& out, const ApiEvent& event, const Index& index)
{
    EmitSummary(out, event.Doc, "            ");
    out += "            /// <remarks>" + std::string(CSharpDeliveryRule(event.Delivery)) + "</remarks>\n";
    out += "            public event " + CSharpHandlerType(event.PayloadId, index) + " " + event.Name + "\n            {\n";
    out += "                add => " + SubscribeName(event) + "(m_Entity, value);\n";
    out += "                remove => " + UnsubscribeName(event) + "(m_Entity, value);\n";
    out += "            }\n";
}

// The two helpers an event's accessors call, and the function the native side calls per event.
void EmitEventPlumbing(std::string& out, const ApiModule& module, const ApiEvent& event, const Index& index)
{
    const std::string handler = CSharpHandlerType(event.PayloadId, index);
    const std::string label = FunctionLabel(module, event.Name);
    const std::string subscribe = SubscribeName(event);
    const std::string unsubscribe = UnsubscribeName(event);
    const std::string payload = index.Records.at(event.PayloadId)->Name;

    out += "\n        private static void " + subscribe + "(Entity scope, " + handler + "? handler)\n        {\n";
    out += "            if (handler == null)\n                return;\n";
    out += "            nint token = ScriptSubscriptionRegistry.Add(\"" + label + "\", scope.Id, handler);\n";
    out += "            ScriptCallbackData callback = new ScriptCallbackData((void*)(delegate* unmanaged[Cdecl]<" + payload +
           "*, void*, void>)&" + CSharpTrampoline(event) + ", token);\n";
    out += "            ulong subscription = 0;\n";
    out += "            int code = " + CSharpPointerField(subscribe) + "(scope.Id, &callback, &subscription);\n";
    out += "            if (code != 0)\n            {\n                ScriptSubscriptionRegistry.Discard(token);\n";
    out += "                if (code != BindingErrors.NotInitialized)\n                    BindingErrors.Throw(code, \"" +
           FunctionLabel(module, subscribe) + "\");\n                return;\n            }\n";
    out += "            ScriptSubscriptionRegistry.Bind(token, subscription, " + CSharpPointerField(unsubscribe) + ");\n        }\n";

    out += "\n        private static void " + unsubscribe + "(Entity scope, " + handler + "? handler)\n        {\n";
    out += "            if (handler == null || !ScriptSubscriptionRegistry.TryFind(\"" + label +
           "\", scope.Id, handler, out nint token, out ulong subscription))\n                return;\n";
    out += "            int code = " + CSharpPointerField(unsubscribe) + "(subscription);\n";
    out += "            if (code == BindingErrors.NotInitialized)\n                ScriptSubscriptionRegistry.Discard(token);\n";
    out += "            else if (code != 0)\n                BindingErrors.Throw(code, \"" + FunctionLabel(module, unsubscribe) + "\");\n        }\n";

    out += "\n        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]\n";
    out += "        private static void " + CSharpTrampoline(event) + "(" + payload + "* payload, void* user)\n        {\n";
    out += "            if (payload == null || ScriptSubscriptionRegistry.Handler((nint)user) is not " + handler + " handler)\n";
    out += "                return;\n";
    out += "            try\n            {\n                handler(in *payload);\n            }\n";
    out += "            catch (Exception error)\n            {\n                ScriptSubscriptionRegistry.Report(\"" + label +
           "\", error);\n            }\n        }\n";
}

void EmitView(std::string& out, const ApiModule& module, const std::vector<const ApiFunction*>& members, const Index& index)
{
    out += "\n        /// <summary>\n        /// The " + module.Name + " script API of one entity: `entity." + CSharpViewProperty(module) +
           "`.\n        /// </summary>\n";
    out += "        public readonly struct " + std::string(CSharpViewType()) + "\n        {\n";
    out += "            private readonly Entity m_Entity;\n\n";
    out += "            internal " + std::string(CSharpViewType()) + "(Entity entity)\n            {\n                m_Entity = entity;\n            }\n\n";
    out += "            /// <summary>\n            /// The entity this view reads and changes.\n            /// </summary>\n";
    out += "            public Entity Entity => m_Entity;\n";
    for (const ApiFunction* function : members)
    {
        out += "\n";
        EmitMethod(out, module, *function, index, true);
    }
    for (const ApiEvent& event : module.Events)
    {
        out += "\n";
        EmitViewEvent(out, event, index);
    }
    out += "        }\n";
}

} // namespace

std::string Emit(const ApiModule& module, const Index& index)
{
    std::vector<const ApiFunction*> statics;
    std::vector<const ApiFunction*> members;
    for (const ApiFunction& function : module.Functions)
        (IsEntityMember(function) ? members : statics).push_back(&function);
    const bool hasView = !members.empty() || !module.Events.empty();

    std::string out;
    out += "// <auto-generated>\n";
    out += "// Generated by ComponentScanner from the " + module.Name +
           " script API (Engine/Scripting/engine-api/" + module.Name + ".json); do not edit.\n";
    out += "// Change the marked header and build the UpdateEngineApi target. The rules are in\n";
    out += "// Tools/ComponentScanner/BindingRules.h.\n";
    out += "// </auto-generated>\n";
    out += "#nullable enable\n\nusing System.Numerics;\n";
    if (!module.Events.empty())
        out += "using System.Runtime.CompilerServices;\n";
    out += "using System.Runtime.InteropServices;\n\n";
    out += "namespace " + std::string(CSharpNamespace()) + "\n{\n";
    for (const ApiEnum& e : module.Enums)
        EmitEnum(out, e, index);
    for (const ApiRecord& record : module.Records)
        EmitRecord(out, record, index);
    std::set<std::string> handlers;
    for (const ApiEvent& event : module.Events)
    {
        if (!handlers.insert(event.PayloadId).second)
            continue;
        const std::string payload = index.Records.at(event.PayloadId)->Name;
        out += "    /// <summary>\n    /// Handles an event that carries a " + payload + ".\n    /// </summary>\n";
        out += "    public delegate void " + CSharpHandlerType(event.PayloadId, index) + "(in " + payload + " e);\n\n";
    }

    out += "    /// <summary>\n    /// The " + module.Name + " script API.\n    /// </summary>\n";
    out += "    public static unsafe partial class " + CSharpModuleClass(module) + "\n    {\n";
    for (const ApiConstant& constant : module.Constants)
        out += "        public const " + CSharpPublicType(constant.Type, index) + " " + CSharpConstant(constant.Name) + " = " +
               CSharpLiteral(constant.Type, constant.Literal, index) + ";\n\n";
    const std::size_t entries = module.Functions.size() + 2 * module.Events.size();
    out += "        private static readonly BindingTable s_Table = BindingTable.Acquire(\"" + module.Name + "\", " +
           HexHash(ApiJsonEmitter::ModuleHash(module)) + ", " + std::to_string(entries) + ");\n";
    std::size_t slot = 0;
    for (const ApiFunction& function : module.Functions)
    {
        const std::string type = PointerType(function, index);
        out += "        private static readonly " + type + " " + CSharpPointerField(function.Name) + " = (" + type + ")s_Table[" +
               std::to_string(slot++) + "];\n";
    }
    for (const ApiEvent& event : module.Events)
    {
        out += "        private static readonly " + std::string(kSubscribePointer) + " " + CSharpPointerField(SubscribeName(event)) +
               " = (" + std::string(kSubscribePointer) + ")s_Table[" + std::to_string(slot++) + "];\n";
        out += "        private static readonly " + std::string(kUnsubscribePointer) + " " +
               CSharpPointerField(UnsubscribeName(event)) + " = (" + std::string(kUnsubscribePointer) + ")s_Table[" +
               std::to_string(slot++) + "];\n";
    }
    for (const ApiFunction* function : statics)
    {
        out += "\n";
        EmitMethod(out, module, *function, index, false);
    }
    for (const ApiEvent& event : module.Events)
        EmitEventPlumbing(out, module, event, index);
    if (hasView)
        EmitView(out, module, members, index);
    out += "    }\n}\n";

    if (hasView)
    {
        const std::string view = "global::" + std::string(CSharpNamespace()) + "." + CSharpModuleClass(module) + "." +
                                 std::string(CSharpViewType());
        out += "\nnamespace GameEngine\n{\n    public readonly partial struct Entity\n    {\n";
        out += "        /// <summary>\n        /// The " + module.Name + " script API of this entity.\n        /// </summary>\n";
        out += "        public " + view + " " + CSharpViewProperty(module) + " => new " + view + "(this);\n";
        out += "    }\n}\n";
    }
    return out;
}

} // namespace GameEngine::ComponentScanner::CSharpBindingEmitter
