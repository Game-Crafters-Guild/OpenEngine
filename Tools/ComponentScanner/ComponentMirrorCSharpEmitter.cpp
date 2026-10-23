#include "ComponentMirrorCSharpEmitter.h"

#include "BindingRules.h"
#include "CSharpBindingEmitter.h"

#include <algorithm>
#include <vector>

namespace GameEngine::ComponentScanner::ComponentMirrorCSharpEmitter
{
namespace
{

// The mirrored structs and the enums of the scan, by C++ name.
struct Names
{
    std::map<std::string, const MirrorStruct*> Structs;
    std::map<std::string, const MirrorEnum*> Enums;
};

// The C# type of one element of a field (of the field itself when it is no array).
std::string ElementType(const MirrorField& field, const Names& names)
{
    switch (field.Kind)
    {
    case MirrorKind::Value: return BindingRules::CSharpPublicType(ApiType{field.Value, {}, {}}, BindingRules::Index{});
    case MirrorKind::AssetGuid: return "AssetGuid";
    case MirrorKind::Curve: return "Curve";
    case MirrorKind::BoundingBox: return "BoundingBox";
    case MirrorKind::Text: return "byte";
    case MirrorKind::Enum: return names.Enums.at(field.TypeName)->Name;
    case MirrorKind::Struct: return names.Structs.at(field.TypeName)->Name;
    }
    return {};
}

// The unsigned C# integer of the enum's width, and the suffix of a literal of it.
std::pair<std::string, std::string> UnsignedOfWidth(ApiKind underlying)
{
    switch (underlying)
    {
    case ApiKind::Int8:
    case ApiKind::UInt8: return {"byte", ""};
    case ApiKind::Int16:
    case ApiKind::UInt16: return {"ushort", ""};
    case ApiKind::Int64:
    case ApiKind::UInt64: return {"ulong", "UL"};
    default: return {"uint", "u"};
    }
}

// The setter of an enum member: the value is stored when the enum declares it. For values that
// run from 0 with no gap, one unsigned compare; for every other enum, a pattern over the values.
std::string EnumSetter(const MirrorEnum& e, const std::string& label, const std::string& privateName)
{
    std::vector<const ApiEnumValue*> distinct;
    for (const ApiEnumValue& value : e.Values)
        if (std::none_of(distinct.begin(), distinct.end(), [&](const ApiEnumValue* seen) { return seen->Value == value.Value; }))
            distinct.push_back(&value);
    std::string names;
    for (std::size_t i = 0; i < distinct.size(); ++i)
        names += std::string(i == 0 ? "" : i + 1 == distinct.size() ? " or " : ", ") + distinct[i]->Name;

    std::vector<std::int64_t> sorted;
    for (const ApiEnumValue* value : distinct)
        sorted.push_back(value->Value);
    std::sort(sorted.begin(), sorted.end());
    bool contiguous = !sorted.empty();
    for (std::size_t i = 0; i < sorted.size(); ++i)
        contiguous = contiguous && sorted[i] == static_cast<std::int64_t>(i);

    std::string condition;
    if (contiguous)
    {
        const auto [type, suffix] = UnsignedOfWidth(e.Underlying);
        condition = "(" + type + ")value <= " + std::to_string(sorted.size() - 1) + suffix;
    }
    else
    {
        condition = "value is ";
        for (std::size_t i = 0; i < distinct.size(); ++i)
            condition += std::string(i == 0 ? "" : " or ") + e.Name + "." + distinct[i]->Name;
    }
    return "            set => " + privateName + " = " + condition + "\n                ? value\n"
           "                : throw new ArgumentOutOfRangeException(nameof(value), value, \"" + label + " is " + names + ".\");\n";
}

void EmitField(std::string& out, const MirrorStruct& owner, const MirrorField& field, const Names& names)
{
    const bool readonlyStruct = owner.Class == ComponentClass::EngineOwned;
    const bool getOnly = readonlyStruct || field.ReadOnly;
    const std::string member = MirrorMemberName(field.Name);
    const std::string privateName = "m_" + member;
    const std::string privateField = std::string("private ") + (readonlyStruct ? "readonly " : "");
    const std::string getterReadonly = readonlyStruct ? "" : "readonly ";
    const std::string label = owner.Name + "." + member;
    const std::string offset = "        [FieldOffset(" + std::to_string(field.Offset) + ")]\n";
    const bool wrapped = field.Kind == MirrorKind::Text || (field.Kind == MirrorKind::Enum && field.Extent == 0);

    std::string type = ElementType(field, names);
    if (field.Extent != 0)
    {
        const std::string nested = MirrorArrayType(field.Name);
        const bool publicArray = !field.Hidden && field.Kind != MirrorKind::Text;
        out += "        [InlineArray(" + std::to_string(field.Extent) + ")]\n        " + (publicArray ? "public" : "private") +
               " struct " + nested + "\n        {\n            private " + type + " m_Element;\n        }\n\n";
        type = nested;
    }

    if (field.Hidden || wrapped)
        out += offset + "        " + privateField + type + " " + privateName + ";\n";
    else
        out += offset + "        public " + (getOnly ? "readonly " : "") + type + " " + member + ";\n";
    if (field.Hidden || !wrapped)
        return;

    if (field.Kind == MirrorKind::Text)
    {
        const std::string read = "ComponentText.Read(" + privateName + ")";
        if (getOnly)
            out += "        public " + getterReadonly + "string " + member + " => " + read + ";\n";
        else
            out += "        public string " + member + "\n        {\n            readonly get => " + read + ";\n"
                   "            set => ComponentText.Write(" + privateName + ", value, \"" + label + "\");\n        }\n";
        return;
    }
    if (getOnly)
    {
        out += "        public " + getterReadonly + type + " " + member + " => " + privateName + ";\n";
        return;
    }
    out += "        public " + type + " " + member + "\n        {\n            readonly get => " + privateName + ";\n" +
           EnumSetter(*names.Enums.at(field.TypeName), label, privateName) + "        }\n";
}

void EmitStruct(std::string& out, const MirrorStruct& mirror, const Names& names)
{
    const bool component = mirror.Class != ComponentClass::Element;
    if (!mirror.Doc.empty())
        CSharpBindingEmitter::EmitSummary(out, mirror.Doc, "    ");
    out += "    [StructLayout(LayoutKind.Explicit, Size = " + std::to_string(mirror.Size) + ")]\n";
    if (component)
        out += "    [BuiltInComponent(\"" + mirror.CppName + "\")]\n";
    out += std::string("    public ") + (mirror.Class == ComponentClass::EngineOwned ? "readonly " : "") + "partial struct " +
           mirror.Name + (component ? " : IComponent" : "") + "\n    {\n";
    out += std::string("        /// <summary>\n        /// Creates the ") + (component ? "component" : "value") +
           " with the engine's defaults.\n        /// </summary>\n";
    out += "        public " + mirror.Name + "()\n        {\n            this = global::GameEngine.ECS.ComponentType<" + mirror.Name +
           ">.Default;\n        }\n";
    for (const MirrorField& field : mirror.Fields)
    {
        out += "\n";
        EmitField(out, mirror, field, names);
    }
    out += "    }\n\n";
}

void EmitEnum(std::string& out, const MirrorEnum& e)
{
    if (!e.Doc.empty())
        CSharpBindingEmitter::EmitSummary(out, e.Doc, "    ");
    out += "    public enum " + e.Name + " : " + BindingRules::CSharpPublicType(ApiType{e.Underlying, {}, {}}, BindingRules::Index{}) +
           "\n    {\n";
    for (const ApiEnumValue& value : e.Values)
        out += "        " + value.Name + " = " + std::to_string(value.Value) + ",\n";
    out += "    }\n\n";
}

// One generated file: the header it mirrors and its text so far.
struct File
{
    std::string Header;
    std::string Body;
};

} // namespace

std::map<std::string, std::string> Emit(const ComponentMirrors& mirrors)
{
    Names names;
    for (const MirrorStruct& mirror : mirrors.Structs)
        names.Structs[mirror.CppName] = &mirror;
    for (const MirrorEnum& e : mirrors.Enums)
        names.Enums[e.CppName] = &e;

    std::map<std::string, File> files;
    for (const MirrorEnum& e : mirrors.Enums)
    {
        if (e.Bound)
            continue;
        File& file = files[MirrorCSharpFile(e.Header)];
        file.Header = e.Header;
        EmitEnum(file.Body, e);
    }
    for (const MirrorStruct& mirror : mirrors.Structs)
    {
        if (!HasMirror(mirror.Class))
            continue;
        File& file = files[MirrorCSharpFile(mirror.Header)];
        file.Header = mirror.Header;
        EmitStruct(file.Body, mirror, names);
    }

    std::map<std::string, std::string> result;
    for (auto& [name, file] : files)
    {
        std::string text = "// <auto-generated>\n// Generated by ComponentScanner from " + file.Header +
                           "; do not edit. The rules are in\n// Tools/ComponentScanner/ComponentMirror.h.\n// </auto-generated>\n";
        text += "#nullable enable\n\nusing System;\nusing System.Numerics;\nusing System.Runtime.CompilerServices;\n"
                "using System.Runtime.InteropServices;\n\nnamespace GameEngine.Scripting\n{\n";
        file.Body.pop_back(); // the blank line after the last type
        text += file.Body + "}\n";
        result[name] = std::move(text);
    }
    return result;
}

} // namespace GameEngine::ComponentScanner::ComponentMirrorCSharpEmitter
