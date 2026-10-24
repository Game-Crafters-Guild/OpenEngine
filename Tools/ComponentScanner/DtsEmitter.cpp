#include "DtsEmitter.h"

#include "NumberFormat.h"

#include <charconv>
#include <set>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner::DtsEmitter
{
namespace
{

constexpr int kTupleExtents[] = {2, 3, 4, 9, 16};

std::string_view ImportedViewType(FieldKind kind)
{
    switch (kind)
    {
    case FieldKind::Vec2: return "Vec2";
    case FieldKind::Vec3: return "Vec3";
    case FieldKind::Vec4: return "Vec4";
    case FieldKind::Quat: return "Quat";
    case FieldKind::Color: return "Color";
    case FieldKind::AssetGuid: return "AssetRef";
    case FieldKind::EntityHandle: return "Entity";
    default: return {};
    }
}

std::string_view ScalarType(FieldKind kind)
{
    switch (kind)
    {
    case FieldKind::Bool:
        return "boolean";
    case FieldKind::Int8:
    case FieldKind::Int16:
    case FieldKind::Int32:
    case FieldKind::UInt8:
    case FieldKind::UInt16:
    case FieldKind::UInt32:
    case FieldKind::RenderLayerMask: // the layer word, bit i = layer i
    case FieldKind::Float:
    case FieldKind::Double:
        return "number";
    case FieldKind::Int64:
    case FieldKind::UInt64:
        return "bigint";
    default:
        return ImportedViewType(kind);
    }
}

// A literal array extent made only of digits.
std::optional<int> LiteralExtent(const std::string& extent)
{
    if (extent.empty() || extent[0] == '-')
        return std::nullopt;
    int value = 0;
    const auto [ptr, ec] = std::from_chars(extent.data(), extent.data() + extent.size(), value);
    if (ec != std::errc{} || ptr != extent.data() + extent.size())
        return std::nullopt;
    return value;
}

std::string FieldType(const WebField& f)
{
    if (f.Kind == FieldKind::String)
        return "string";
    std::string element = f.Enum ? f.Enum->Name : std::string(ScalarType(f.Kind));
    if (f.Kind == FieldKind::EntityHandle)
        element = f.ArrayExtent ? "(Entity | null)" : "Entity | null";
    if (!f.ArrayExtent)
        return element;
    if (f.Kind == FieldKind::Float && !f.Enum)
    {
        if (const std::optional<int> extent = LiteralExtent(*f.ArrayExtent))
        {
            for (int tuple : kTupleExtents)
            {
                if (tuple != *extent)
                    continue;
                std::string text = "[";
                for (int i = 0; i < tuple; ++i)
                {
                    if (i > 0)
                        text += ", ";
                    text += element;
                }
                return text + "]";
            }
        }
    }
    return element + "[]";
}

// "*/" inside a tooltip would end the JSDoc comment.
std::string EscapeCommentClose(const std::string& text)
{
    std::string result;
    std::size_t pos = 0;
    while (true)
    {
        const std::size_t hit = text.find("*/", pos);
        if (hit == std::string::npos)
        {
            result.append(text, pos, std::string::npos);
            return result;
        }
        result.append(text, pos, hit - pos);
        result += "*\\/";
        pos = hit + 2;
    }
}

// The tooltip, then the range, as the field's JSDoc; no comment for an unmarked field.
void AppendDoc(std::string& out, const WebField& f)
{
    std::vector<std::string> lines;
    if (f.Tooltip)
        lines.push_back(EscapeCommentClose(*f.Tooltip));
    if (f.Range)
        lines.push_back(f.Range->Max ? "Range: " + NumberFormat::ShortestRoundTrip(f.Range->Min) + " to " +
                                           NumberFormat::ShortestRoundTrip(*f.Range->Max) + "."
                                     : "Minimum: " + NumberFormat::ShortestRoundTrip(f.Range->Min) + ".");
    if (lines.empty())
        return;
    if (lines.size() == 1)
    {
        out += "    /** " + lines[0] + " */\n";
        return;
    }
    out += "    /**\n";
    for (const std::string& line : lines)
        out += "     * " + line + "\n";
    out += "     */\n";
}

} // namespace

std::string Emit(const WebComponentModel& model)
{
    std::string out;
    out += "// Generated from the engine's component headers by ComponentScanner --emit-dts. Do not edit.\n";
    out += "// One interface and one ComponentType value per reflected component; & NoAdd marks the ones a page\n";
    out += "// cannot add or remove. Each field's type follows its reflected kind.\n";

    // An engine-internal component is no game API: it and the enums only it uses stay out.
    std::vector<const WebComponent*> components;
    std::set<const EnumDecl*> enums;
    std::set<std::string> imports;
    for (const WebComponent& c : model.Components)
    {
        if (c.EngineInternal)
            continue;
        components.push_back(&c);
        imports.insert("ComponentType");
        if (c.NoAdd)
            imports.insert("NoAdd");
        for (const WebField& f : c.Fields)
        {
            if (f.Enum)
                enums.insert(f.Enum);
            else if (const std::string_view view = ImportedViewType(f.Kind); !view.empty())
                imports.insert(std::string(view));
        }
    }
    if (!imports.empty())
    {
        out += "\nimport type { ";
        bool first = true;
        for (const std::string& name : imports)
        {
            if (!first)
                out += ", ";
            first = false;
            out += name;
        }
        out += " } from './opengine';\n";
    }

    for (const EnumDecl* e : model.Enums)
    {
        if (enums.count(e) == 0)
            continue;
        out += "\nexport type " + e->Name + " = ";
        for (std::size_t i = 0; i < e->Members.size(); ++i)
        {
            if (i > 0)
                out += " | ";
            out += "'" + e->Members[i].Name + "'";
        }
        out += ";\n";
    }

    for (const WebComponent* c : components)
    {
        out += "\nexport interface " + c->Name + " {\n";
        for (const WebField& f : c->Fields)
        {
            AppendDoc(out, f);
            out += "    " + std::string(f.ReadOnly ? "readonly " : "") + f.TsName + ": " + FieldType(f) + ";\n";
        }
        out += "}\n";
        out += "export declare const " + c->Name + ": ComponentType<" + c->Name + ">" + (c->NoAdd ? " & NoAdd" : "") + ";\n";
    }
    return out;
}

} // namespace GameEngine::ComponentScanner::DtsEmitter
