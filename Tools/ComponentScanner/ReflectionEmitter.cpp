#include "ReflectionEmitter.h"

#include "NumberFormat.h"

#include <map>

namespace GameEngine::ComponentScanner::ReflectionEmitter
{
namespace
{

// "0" -> "0.0f", "1.5" -> "1.5f", "1E-05" -> "1E-05f": a float literal needs a point or an
// exponent before the suffix.
std::string CppFloatLiteral(double value)
{
    std::string text = NumberFormat::ShortestRoundTrip(value);
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    return text + "f";
}

// The GE_REFLECT_FIELD_RANGE argument list ("Min, Max"); an omitted Max binds an open maximum.
std::string FieldRangeArgs(const FieldRange& range)
{
    const std::string max = range.Max ? CppFloatLiteral(*range.Max) : "::std::numeric_limits<float>::infinity()";
    return CppFloatLiteral(range.Min) + ", " + max;
}

std::string CppStringLiteral(const std::string& value)
{
    std::string out = "\"";
    for (char c : value)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
        {
            const unsigned char code = static_cast<unsigned char>(c);
            if (code < 0x20 || code == 0x7F)
            {
                // A C++ numeric escape is octal, three digits: ESC is "\033", DEL "\177".
                out += '\\';
                out += static_cast<char>('0' + code / 64);
                out += static_cast<char>('0' + (code / 8) % 8);
                out += static_cast<char>('0' + code % 8);
            }
            else
            {
                out += c;
            }
            break;
        }
        }
    }
    out += '"';
    return out;
}

std::string JoinNames(const std::vector<const ReflectedStruct*>& structs)
{
    std::string names;
    for (const ReflectedStruct* r : structs)
    {
        if (!names.empty())
            names += ", ";
        names += r->Name;
    }
    return names;
}

// Emit one cap-free registration block: a BEGIN, one FIELD per member, an END (addable) or
// END_NO_ADD (kept out of the Add Component menu), followed by the GE_REFLECT_* lines for
// fields and structs carrying metadata markers.
std::string FormatRegisterBlock(const std::string& qualified, const ReflectedStruct& r, const EnumTable& enums)
{
    const char* endMacro = r.NoAdd ? "GE_REGISTER_COMPONENT_END_NO_ADD" : "GE_REGISTER_COMPONENT_END";

    std::string out;
    out += "GE_REGISTER_COMPONENT_BEGIN(" + qualified + ")\n";
    for (const ReflectedField& f : r.Fields)
        out += "    GE_REGISTER_COMPONENT_FIELD(" + qualified + ", " + f.Name + ")\n";
    out += std::string(endMacro) + "(" + qualified + ")\n";

    for (const ReflectedField& f : r.Fields)
        if (const std::optional<std::string> flagExpr = FieldFlagExpr(f))
            out += "GE_REFLECT_FIELD_FLAGS(" + qualified + ", " + f.Name + ", " + *flagExpr + ");\n";

    for (const ReflectedField& f : r.Fields)
        if (f.Tooltip)
            out += "GE_REFLECT_FIELD_TOOLTIP(" + qualified + ", " + f.Name + ", " + CppStringLiteral(*f.Tooltip) + ");\n";

    for (const ReflectedField& f : r.Fields)
        if (f.Range)
            out += "GE_REFLECT_FIELD_RANGE(" + qualified + ", " + f.Name + ", " + FieldRangeArgs(*f.Range) + ");\n";

    if (r.DoNotSerialize)
        out += "GE_REFLECT_COMPONENT_DONOTSERIALIZE(" + qualified + ");\n";

    if (r.EditorOnly)
        out += "GE_REFLECT_COMPONENT_EDITORONLY(" + qualified + ");\n";

    // Bind enum-typed fields to their value-name table (emitted above) for name serialization.
    for (const ReflectedField& f : r.Fields)
        if (const EnumDecl* ed = enums.Match(f.Type))
            out += "GE_REFLECT_ENUM_FIELD(" + qualified + ", " + f.Name + ", " + ed->Name + ")\n";

    return out;
}

// Appends one FieldFlags enumerator to a "A | B" expression.
void AppendFlag(std::string& flags, const char* flag)
{
    if (!flags.empty())
        flags += " | ";
    flags += flag;
}

// Drop trailing whitespace and end with exactly one newline.
std::string TrimEndWithNewline(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.pop_back();
    return text + "\n";
}

} // namespace

std::optional<std::string> FieldFlagExpr(const ReflectedField& field)
{
    std::string flags;
    if (field.Hidden)
        AppendFlag(flags, "::GameEngine::ECS::FieldFlags::Hidden");
    if (field.ReadOnly)
        AppendFlag(flags, "::GameEngine::ECS::FieldFlags::ReadOnly");
    if (field.Transient)
        AppendFlag(flags, "::GameEngine::ECS::FieldFlags::Transient");
    if (flags.empty())
        return std::nullopt;
    return flags;
}

std::string EmitGeneratedCpp(const std::vector<ReflectedStruct>& reflected, const std::vector<std::string>& includes,
                             bool userMode, const EnumTable& enums)
{
    std::map<std::string, std::vector<const ReflectedStruct*>> byHeader;
    for (const ReflectedStruct& r : reflected)
        byHeader[r.Header].push_back(&r);

    std::string out;
    out += "// AUTO-GENERATED by ComponentScanner -- do not edit by hand.\n";
    out += "// Regenerate by running the ComponentScanner tool over the component headers.\n";
    out += "// One cap-free GE_REGISTER_COMPONENT_BEGIN/_FIELD/_END block per reflected\n";
    out += "// component struct (+ GE_REFLECT_FIELD_FLAGS / GE_REFLECT_FIELD_TOOLTIP metadata).\n";
    out += "\n";
    // User DLLs use the PCH-light registration header: the same GE_REGISTER_COMPONENT_*
    // macros, but the factory adds via type-erased bytes (no World::AddComponentImmediate<T>
    // template instantiation). The engine's own generated TU keeps the typed path.
    out += userMode
               ? "#include \"Components/ComponentRegistrationLite.h\"  // GE_REGISTER_COMPONENT_BEGIN/_FIELD/_END (type-erased)\n"
               : "#include \"Components/ComponentRegistration.h\"  // GE_REGISTER_COMPONENT_BEGIN/_FIELD/_END\n";
    out += "\n";

    for (const std::string& inc : includes)
    {
        const auto it = byHeader.find(inc);
        if (it != byHeader.end() && !it->second.empty())
            out += "#include \"" + inc + "\"  // " + JoinNames(it->second) + "\n";
        else
            out += "#include \"" + inc + "\"\n";
    }

    out += "\n";

    // Enum tables: one constexpr name<->value table per enum actually used by a reflected
    // field, emitted before the component blocks (the GE_REFLECT_ENUM_FIELD lines reference
    // GeEnumTables::<EnumName>). Sorted by name for a stable generated file.
    std::map<std::string, const EnumDecl*> usedEnums;
    for (const ReflectedStruct& r : reflected)
        for (const ReflectedField& f : r.Fields)
            if (const EnumDecl* ed = enums.Match(f.Type))
                usedEnums[ed->Name] = ed;

    if (!usedEnums.empty())
    {
        out += "// --- enum value-name tables (constexpr; bound to fields below) ---\n";
        for (const auto& [name, ed] : usedEnums)
        {
            out += "GE_REFLECT_ENUM_TABLE_BEGIN(" + ed->Name + ")\n";
            for (const EnumMember& m : ed->Members)
                out += "    GE_REFLECT_ENUM_TABLE_VALUE(\"" + m.Name + "\", " + std::to_string(m.Value) + ")\n";
            out += "GE_REFLECT_ENUM_TABLE_END()\n";
        }
        out += "\n";
    }

    for (const std::string& inc : includes)
    {
        const auto it = byHeader.find(inc);
        if (it == byHeader.end())
            continue;
        for (const ReflectedStruct* r : it->second)
        {
            out += FormatRegisterBlock(QualifiedName(r->Namespace, r->Name), *r, enums);
            out += "\n";
        }
    }

    return TrimEndWithNewline(std::move(out));
}

std::string EmitSystemRegistrationCpp(const std::vector<ReflectedSystem>& systems,
                                      const std::vector<std::string>& includes)
{
    std::map<std::string, std::vector<const ReflectedSystem*>> byHeader;
    for (const ReflectedSystem& s : systems)
        byHeader[s.Header].push_back(&s);

    std::string out;
    out += "// AUTO-GENERATED by ComponentScanner --detect-system-base -- do not edit by hand.\n";
    out += "// One RegisterUserSystem(new UserSystemAdapter<T>) per ECS::SystemBase-derived user\n";
    out += "// system; the adapter forwards the system's optional OnStart/OnUpdate/OnDestroy.\n";
    out += "\n";
    out += "#include \"GameSDK/System.h\"  // UserSystemAdapter + RegisterUserSystem + the typed Query API\n";
    out += "\n";

    for (const std::string& inc : includes)
    {
        std::string names;
        for (const ReflectedSystem* s : byHeader[inc])
        {
            if (!names.empty())
                names += ", ";
            names += s->Name;
        }
        out += "#include \"" + inc + "\"  // " + names + "\n";
    }
    out += "\n";

    out += "namespace {\n";
    int n = 0;
    for (const std::string& inc : includes)
    {
        for (const ReflectedSystem* s : byHeader[inc])
        {
            const std::string q = QualifiedName(s->Namespace, s->Name);
            out += "[[maybe_unused]] const bool ge_user_system_" + std::to_string(n++) + " =\n";
            out += "    ::GameEngine::NativeScripting::RegisterUserSystem(\n";
            out += "        new ::GameEngine::ECS::UserSystemAdapter<" + q + ">(\"" + q + "\"));\n";
        }
    }
    out += "} // namespace\n";

    return TrimEndWithNewline(std::move(out));
}

} // namespace GameEngine::ComponentScanner::ReflectionEmitter
