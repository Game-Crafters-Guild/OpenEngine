#include "ApiResolver.h"

#include "Lexical.h"
#include "NumberFormat.h"
#include "ReflectedModel.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace GameEngine::ComponentScanner
{
namespace
{

using namespace Lexical;

constexpr std::string_view kDefaultFix = "literal defaults, or a constant in this header";

// A literal of a scalar kind in its one spelling: true or false, a decimal integer, the
// shortest float or double (NumberFormat).
NumberFormat::CanonicalNumber CanonicalLiteral(std::string_view text, ApiKind kind)
{
    const std::string value = Trim(text);
    if (kind == ApiKind::Bool)
        return value == "true" || value == "false" ? NumberFormat::CanonicalNumber{NumberFormat::LiteralStatus::Ok, value}
                                                   : NumberFormat::CanonicalNumber{};
    if (ApiKindResolver::IsInteger(kind) || kind == ApiKind::StringId)
        return NumberFormat::CanonicalInteger(value);
    if (kind == ApiKind::Float || kind == ApiKind::Angle)
        return NumberFormat::CanonicalFloat(value);
    if (kind == ApiKind::Double)
        return NumberFormat::CanonicalDouble(value);
    return {};
}

bool IsQualifiedIdentifier(std::string_view text)
{
    std::string_view t = TrimView(text);
    if (StartsWith(t, "::"))
        t.remove_prefix(2);
    if (t.empty())
        return false;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t sep = t.find("::", start);
        if (!IsIdentifier(t.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start)))
            return false;
        if (sep == std::string_view::npos)
            return true;
        start = sep + 2;
    }
}

// The memory a default-constructed value of a Mathematics or color kind holds, component by
// component: zeros for the vectors, the identity quaternion (x, y, z, w) and matrix (column
// major), and opaque black for a color.
std::vector<std::string> DefaultComponents(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Vec2:
        return {"0", "0"};
    case ApiKind::Vec3:
        return {"0", "0", "0"};
    case ApiKind::Vec4:
        return {"0", "0", "0", "0"};
    case ApiKind::Quat:
    case ApiKind::ColorLinear:
        return {"0", "0", "0", "1"};
    case ApiKind::Mat4:
        return {"1", "0", "0", "0", "0", "1", "0", "0", "0", "0", "1", "0", "0", "0", "0", "1"};
    default:
        return {};
    }
}

bool IsScalarKind(ApiKind kind)
{
    return kind == ApiKind::Bool || ApiKindResolver::IsInteger(kind) || kind == ApiKind::Float ||
           kind == ApiKind::Double || kind == ApiKind::Angle || kind == ApiKind::StringId || kind == ApiKind::Enum;
}

// The type text without a leading `const` and a trailing `&`.
std::string BareType(std::string_view text)
{
    std::string t = Trim(text);
    while (!t.empty() && t.back() == '&')
        t = Trim(std::string_view(t).substr(0, t.size() - 1));
    if (StartsWith(t, "const "))
        t = Trim(std::string_view(t).substr(6));
    return t;
}

// The comma-separated items of a brace list's inside, trimmed.
std::vector<std::string> BraceItems(std::string_view inner)
{
    std::vector<std::string> items;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t comma = inner.find(',', start);
        items.push_back(Trim(inner.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
        if (comma == std::string_view::npos)
            return items;
        start = comma + 1;
    }
}

} // namespace

ApiResolver::ApiResolver(const std::vector<std::unique_ptr<ModuleDraft>>& modules) : m_Modules(modules)
{
}

ApiDescription ApiResolver::Resolve()
{
    CollectNamedTypes();
    ApiDescription description;
    for (const std::unique_ptr<ModuleDraft>& module : m_Modules)
        if (std::optional<ApiModule> resolved = ResolveModule(*module))
            description.Modules.push_back(std::move(*resolved));
    return description;
}

void ApiResolver::Refuse(const std::string& file, int line, std::string problem, std::string fix)
{
    m_Refusals.push_back(ApiRefusal{file, line, std::move(problem), std::move(fix)});
}

void ApiResolver::CollectNamedTypes()
{
    for (const std::unique_ptr<ModuleDraft>& module : m_Modules)
    {
        for (const Draft& draft : module->Drafts)
        {
            const ApiKind kind = draft.Kind == DraftKind::Record   ? ApiKind::Record
                                 : draft.Kind == DraftKind::Enum   ? ApiKind::Enum
                                 : draft.Kind == DraftKind::Handle ? ApiKind::Handle
                                                                   : ApiKind::Void;
            if (kind != ApiKind::Void)
                m_Named.push_back(ApiNamedType{kind, module->Name + "." + draft.Name, draft.CppName});
        }
    }
}

// The module and draft that declare the named type `id`.
std::pair<const ModuleDraft*, const Draft*> ApiResolver::FindNamed(const std::string& id) const
{
    for (const std::unique_ptr<ModuleDraft>& module : m_Modules)
        for (const Draft& draft : module->Drafts)
            if (module->Name + "." + draft.Name == id &&
                (draft.Kind == DraftKind::Record || draft.Kind == DraftKind::Enum || draft.Kind == DraftKind::Handle))
                return {module.get(), &draft};
    return {nullptr, nullptr};
}

ApiSource ApiResolver::SourceOf(const HeaderSource& header, int line)
{
    return ApiSource{header.Include, line, header.File};
}

const HeaderSource& ApiResolver::HeaderOf(const ModuleDraft& module, const Draft& draft)
{
    return module.Headers[draft.HeaderIndex];
}

ApiLookup ApiResolver::LookupFrom(std::string_view scope) const
{
    return ApiLookup{m_Named, scope};
}

std::optional<ApiEnum> ApiResolver::ResolveEnum(const std::string& id)
{
    if (const auto it = m_Enums.find(id); it != m_Enums.end())
        return it->second;
    const auto [module, draft] = FindNamed(id);
    std::optional<ApiEnum> result = ReadEnum(*module, *draft);
    m_Enums[id] = result;
    return result;
}

std::optional<ApiEnum> ApiResolver::ReadEnum(const ModuleDraft& module, const Draft& draft)
{
    const HeaderSource& header = HeaderOf(module, draft);
    const EnumDecl& decl = header.Enums[draft.EnumIndex];
    if (!decl.Scoped)
    {
        Refuse(header.File, draft.Line, "a plain enum cannot be bound", "declare it as an enum class with an explicit underlying type");
        return std::nullopt;
    }
    if (!decl.Underlying)
    {
        Refuse(header.File, draft.Line, "the enum class `" + decl.Name + "` has no explicit underlying type",
               "write one: `enum class " + decl.Name + " : std::uint8_t`");
        return std::nullopt;
    }
    DeclarationProblem problem;
    const std::optional<ApiType> underlying = ApiKindResolver::ResolveValue(*decl.Underlying, LookupFrom(draft.Scope), problem);
    if (!underlying || !ApiKindResolver::IsInteger(underlying->Kind))
    {
        Refuse(header.File, draft.Line, "the underlying type `" + *decl.Underlying + "` is not a fixed-width integer",
               "use std::uint8_t, std::int32_t or another fixed-width integer");
        return std::nullopt;
    }
    const std::uint32_t bits = ApiKindResolver::SizeOf(underlying->Kind) * 8;
    const bool isSigned = underlying->Kind == ApiKind::Int8 || underlying->Kind == ApiKind::Int16 ||
                          underlying->Kind == ApiKind::Int32 || underlying->Kind == ApiKind::Int64;
    ApiEnum result;
    result.Name = draft.Name;
    result.CppName = draft.CppName;
    result.Source = SourceOf(header, draft.Line);
    result.Doc = draft.Doc;
    result.Underlying = underlying->Kind;
    result.Flags = draft.Flags;
    for (const EnumMember& member : decl.Members)
    {
        const bool fits = bits == 64 ? (isSigned || member.Value >= 0)
                          : isSigned ? member.Value >= -(std::int64_t(1) << (bits - 1)) && member.Value < (std::int64_t(1) << (bits - 1))
                                     : member.Value >= 0 && member.Value < (std::int64_t(1) << bits);
        if (!fits)
        {
            Refuse(header.File, draft.Line, "the value of `" + member.Name + "` does not fit `" + *decl.Underlying + "`",
                   "choose a wider underlying type or a smaller value");
            return std::nullopt;
        }
        result.Values.push_back(ApiEnumValue{member.Name, member.Value});
    }
    return result;
}

std::optional<ApiRecord> ApiResolver::ResolveRecord(const std::string& id)
{
    if (const auto it = m_Records.find(id); it != m_Records.end())
        return it->second;
    const auto [module, draft] = FindNamed(id);
    if (!m_Resolving.insert(id).second)
    {
        Refuse(HeaderOf(*module, *draft).File, draft->Line, "the record `" + draft->Name + "` contains itself", "remove the field that holds it");
        return std::nullopt;
    }
    std::optional<ApiRecord> result = ReadRecord(*module, *draft);
    m_Resolving.erase(id);
    m_Records[id] = result;
    return result;
}

// The size and alignment of a resolved type, or nullopt when a record or enum it names
// failed to resolve.
std::optional<std::pair<std::uint32_t, std::uint32_t>> ApiResolver::LayoutOf(const ApiType& type)
{
    if (type.Kind == ApiKind::Record)
    {
        const std::optional<ApiRecord> record = ResolveRecord(type.TypeId);
        if (!record)
            return std::nullopt;
        return std::make_pair(record->Size, record->Align);
    }
    if (type.Kind == ApiKind::Enum)
    {
        const std::optional<ApiEnum> e = ResolveEnum(type.TypeId);
        if (!e)
            return std::nullopt;
        return std::make_pair(ApiKindResolver::SizeOf(e->Underlying), ApiKindResolver::AlignOf(e->Underlying));
    }
    return std::make_pair(ApiKindResolver::SizeOf(type.Kind), ApiKindResolver::AlignOf(type.Kind));
}

std::optional<ApiRecord> ApiResolver::ReadRecord(const ModuleDraft& module, const Draft& draft)
{
    const HeaderSource& header = HeaderOf(module, draft);
    const ParsedStruct& parsed = header.Structs[draft.StructIndex];
    if (!TrimView(parsed.Bases).empty())
    {
        Refuse(header.File, draft.Line, "a record has no base class", "copy the base's fields into the record");
        return std::nullopt;
    }
    if (parsed.HasVirtual)
    {
        Refuse(header.File, draft.Line, "a record has no virtual member", "move the behavior into a free function");
        return std::nullopt;
    }
    if (parsed.UnexpandedMacro)
    {
        Refuse(header.File, draft.Line, "the macro `" + *parsed.UnexpandedMacro + "` hides the record's fields",
               "write the fields out in the struct");
        return std::nullopt;
    }
    if (parsed.Fields.empty())
    {
        Refuse(header.File, draft.Line, "the record has no fields", "give it a field, or remove the marker");
        return std::nullopt;
    }

    ApiRecord record;
    record.Name = draft.Name;
    record.CppName = draft.CppName;
    record.Source = SourceOf(header, draft.Line);
    record.Doc = draft.Doc;
    bool ok = true;
    std::uint32_t offset = 0;
    for (const FieldDecl& field : parsed.Fields)
    {
        std::optional<ApiField> resolved = ReadField(module, draft, field);
        if (!resolved)
        {
            ok = false;
            continue;
        }
        const std::optional<std::pair<std::uint32_t, std::uint32_t>> layout = LayoutOf(resolved->Type);
        if (!layout)
        {
            ok = false;
            continue;
        }
        const auto [size, align] = *layout;
        offset = (offset + align - 1) / align * align;
        resolved->Offset = offset;
        offset += size;
        record.Align = std::max(record.Align, align);
        record.Fields.push_back(std::move(*resolved));
    }
    if (!ok)
        return std::nullopt;
    record.Size = (offset + record.Align - 1) / record.Align * record.Align;
    return record;
}

std::optional<ApiField> ApiResolver::ReadField(const ModuleDraft& module, const Draft& record, const FieldDecl& field)
{
    const HeaderSource& header = HeaderOf(module, record);
    if (field.ArrayExtent)
    {
        Refuse(header.File, field.DeclLine, "the array field `" + field.Name + "` cannot be bound",
               "give each element its own field, or use a Mathematics vector");
        return std::nullopt;
    }
    DeclarationProblem problem;
    std::optional<ApiType> type = ApiKindResolver::ResolveValue(field.Type, LookupFrom(record.Scope), problem);
    if (!type)
    {
        Refuse(header.File, field.DeclLine, "field `" + field.Name + "`: " + problem.Problem, problem.Fix);
        return std::nullopt;
    }
    if (type->Kind == ApiKind::String || type->Kind == ApiKind::Span || type->Kind == ApiKind::Void)
    {
        Refuse(header.File, field.DeclLine, "the field `" + field.Name + "` holds text or a span, which a record cannot",
               "keep text and lists as function parameters");
        return std::nullopt;
    }

    ApiField result;
    result.Name = field.Name;
    result.Type = *type;
    if (!ReadFieldDefault(module, record, field, result))
        return std::nullopt;
    return result;
}

bool ApiResolver::ReadFieldDefault(const ModuleDraft& module, const Draft& record, const FieldDecl& field, ApiField& result)
{
    const HeaderSource& header = HeaderOf(module, record);
    const ApiKind kind = result.Type.Kind;
    const std::string initializer = field.Initializer ? Trim(*field.Initializer) : std::string();
    const bool braced = StartsWith(initializer, "{") && EndsWith(initializer, "}");
    const std::string inner = braced ? Trim(std::string_view(initializer).substr(1, initializer.size() - 2)) : initializer;

    if (const std::vector<std::string> defaults = DefaultComponents(kind); !defaults.empty())
    {
        // No initializer, or {}: the type's default constructor.
        if (initializer.empty() || (braced && inner.empty()))
        {
            result.Default = defaults;
            return true;
        }
        if (kind == ApiKind::Mat4)
        {
            Refuse(header.File, field.DeclLine, "the matrix field `" + field.Name + "` has an initializer the description cannot read",
                   "leave the field without an initializer (the identity)");
            return false;
        }
        // The constructor's parameters: x, y[, z[, w]] for a vector, w, x, y, z for a
        // quaternion, and r, g, b[, a] for a color (a defaults to 1).
        std::vector<std::string> items = braced ? BraceItems(inner) : std::vector<std::string>();
        const bool counts = kind == ApiKind::ColorLinear ? (items.size() == 3 || items.size() == 4) : items.size() == defaults.size();
        std::vector<std::string> components;
        for (const std::string& item : items)
            if (NumberFormat::CanonicalNumber value = NumberFormat::CanonicalFloat(item);
                value.Status == NumberFormat::LiteralStatus::Ok)
                components.push_back(std::move(value.Text));
        if (!braced || !counts || components.size() != items.size())
        {
            Refuse(header.File, field.DeclLine,
                   "the field `" + field.Name + "` has an initializer the description cannot read",
                   "leave the field without an initializer (the type's default), or write a brace list of number literals in "
                   "the constructor's order");
            return false;
        }
        if (kind == ApiKind::Quat)
            std::rotate(components.begin(), components.begin() + 1, components.end());
        if (kind == ApiKind::ColorLinear && components.size() == 3)
            components.push_back("1");
        result.Default = std::move(components);
        return true;
    }

    if (initializer.empty() || (braced && inner.empty()))
        return true;
    if (kind == ApiKind::Record || kind == ApiKind::Entity || kind == ApiKind::Handle)
    {
        Refuse(header.File, field.DeclLine, "the field `" + field.Name + "` has an initializer its kind cannot carry",
               kind == ApiKind::Record ? "write no initializer or {}: the field takes the record's own defaults"
                                       : "write no initializer or {}: the field starts empty");
        return false;
    }
    DeclarationProblem problem;
    std::optional<ApiDefault> value = ReadScalarDefault(module, header, inner, result.Type, problem);
    if (!value)
    {
        Refuse(header.File, field.DeclLine, "field `" + field.Name + "`: " + problem.Problem, problem.Fix);
        return false;
    }
    result.Default.push_back(value->Literal);
    result.DefaultConstantId = value->ConstantId;
    return true;
}

// A scalar default: a literal of the kind, an enumerator of the enum, or a constexpr
// scalar of the same header, in its one spelling (CanonicalLiteral).
std::optional<ApiDefault> ApiResolver::ReadScalarDefault(const ModuleDraft& module, const HeaderSource& header, std::string_view text,
                                                         const ApiType& type, DeclarationProblem& outProblem)
{
    const std::string value = Trim(text);
    const ApiKind kind = type.Kind;
    NumberFormat::CanonicalNumber literal = CanonicalLiteral(value, kind);
    if (literal.Status == NumberFormat::LiteralStatus::Ok)
        return ApiDefault{std::move(literal.Text), std::nullopt};
    if (literal.Status == NumberFormat::LiteralStatus::OutOfRange)
    {
        outProblem = DeclarationProblem{"the default `" + value + "` is outside the range of the parameter's kind",
                                        "write a value the kind can hold"};
        return std::nullopt;
    }

    if (kind == ApiKind::Enum)
    {
        const std::optional<ApiEnum> e = ResolveEnum(type.TypeId);
        const std::size_t sep = value.rfind("::");
        const std::string enumerator = sep == std::string::npos ? value : value.substr(sep + 2);
        const std::string qualifier = sep == std::string::npos ? std::string() : value.substr(0, sep);
        if (e && !qualifier.empty() && ApiKindResolver::NamesQualified(qualifier, e->CppName) &&
            std::any_of(e->Values.begin(), e->Values.end(), [&](const ApiEnumValue& v)
                        { return v.Name == enumerator; }))
            return ApiDefault{enumerator, std::nullopt};
        outProblem = DeclarationProblem{"the default `" + value + "` is not an enumerator of the parameter's enum",
                                        "write the enumerator as Enum::Value"};
        return std::nullopt;
    }

    if (IsQualifiedIdentifier(value))
    {
        for (const ParsedConstant& constant : header.Constants)
        {
            const std::string qualified = Qualify(constant.Scope, constant.Name);
            if (!ApiKindResolver::NamesQualified(value, qualified))
                continue;
            DeclarationProblem ignored;
            const std::optional<ApiType> constantType =
                ApiKindResolver::ResolveValue(BareType(constant.Type), LookupFrom(ScopeName(constant.Scope)), ignored);
            if (!constantType || constantType->Kind == ApiKind::Enum || !IsScalarKind(constantType->Kind))
                break;
            const NumberFormat::CanonicalNumber constantValue = CanonicalLiteral(constant.Literal, constantType->Kind);
            const NumberFormat::CanonicalNumber resolved = constantValue.Status == NumberFormat::LiteralStatus::Ok
                                                               ? CanonicalLiteral(constantValue.Text, kind)
                                                               : NumberFormat::CanonicalNumber{};
            if (resolved.Status != NumberFormat::LiteralStatus::Ok)
                break;
            const std::string id = module.Name + "." + constant.Name;
            m_ReferencedConstants[module.Name][id] =
                ApiConstant{constant.Name, qualified, SourceOf(header, constant.Line), *constantType, constantValue.Text};
            return ApiDefault{resolved.Text, id};
        }
    }
    outProblem = DeclarationProblem{"the default `" + value + "` is not a literal of the parameter's kind", std::string(kDefaultFix)};
    return std::nullopt;
}

std::optional<ApiFunction> ApiResolver::ReadFunction(const ModuleDraft& module, const Draft& draft)
{
    const HeaderSource& header = HeaderOf(module, draft);
    const ParsedFunction& fn = draft.Function;
    ApiFunction result;
    result.Name = draft.Name;
    result.CppName = draft.CppName;
    result.Source = SourceOf(header, draft.Line);
    result.Doc = draft.Doc;
    result.Thread = draft.Thread;
    result.Try = draft.Try;

    std::vector<ParsedParam> params = fn.Params;
    if (!ReadContext(header, draft, params, result.Context))
        return std::nullopt;

    if (fn.ReturnType.find('&') != std::string::npos || fn.ReturnType.find('*') != std::string::npos)
    {
        Refuse(header.File, draft.Line, "the function returns `" + fn.ReturnType + "`, an address",
               "return by value, or write the result through one out parameter");
        return std::nullopt;
    }
    DeclarationProblem problem;
    std::optional<ApiType> returns = ApiKindResolver::ResolveValue(BareType(fn.ReturnType), LookupFrom(draft.Scope), problem);
    if (!returns)
    {
        Refuse(header.File, draft.Line, "return type: " + problem.Problem, problem.Fix);
        return std::nullopt;
    }
    if (returns->Kind == ApiKind::String || returns->Kind == ApiKind::Span)
    {
        Refuse(header.File, draft.Line, "text and spans are parameters only", "return a value kind");
        return std::nullopt;
    }
    result.Returns = *returns;

    bool ok = true;
    bool sawDefault = false;
    std::size_t outCount = 0;
    for (const ParsedParam& param : params)
    {
        std::optional<ApiParam> resolved = ReadParam(module, draft, param);
        if (!resolved)
        {
            ok = false;
            continue;
        }
        if (resolved->Out)
        {
            ++outCount;
            if (sawDefault)
            {
                Refuse(header.File, draft.Line, "the out parameter `" + param.Name + "` follows a defaulted parameter",
                       "put the out parameter before every defaulted parameter");
                ok = false;
            }
        }
        sawDefault = sawDefault || resolved->Default.has_value();
        result.Params.push_back(std::move(*resolved));
    }
    if (outCount > 1)
    {
        Refuse(header.File, draft.Line, "the function has " + std::to_string(outCount) + " out parameters",
               "return a record that holds the results, or keep one out parameter");
        ok = false;
    }
    if (draft.Try && (result.Returns.Kind != ApiKind::Bool || outCount != 1))
    {
        Refuse(header.File, draft.Line, "@ge-script-try marks a bool function with one out parameter",
               "return bool and write the answer through one out parameter, or remove the marker");
        ok = false;
    }
    if (!ok)
        return std::nullopt;
    return result;
}

bool ApiResolver::ReadContext(const HeaderSource& header, const Draft& draft, std::vector<ParsedParam>& params,
                              std::optional<std::string>& outContext)
{
    if (!draft.Context)
        return true;
    const std::string type = params.empty() ? std::string() : Trim(params.front().Type);
    if (type.empty() || type.back() != '&' || StartsWith(type, "const ") || EndsWith(type, "&&"))
    {
        Refuse(header.File, draft.Line, "@ge-context needs the system the function reaches as its first parameter, as `System&`",
               "make the first parameter a non-const reference to the system, or remove the marker");
        return false;
    }
    outContext = Trim(std::string_view(type).substr(0, type.size() - 1));
    params.erase(params.begin());
    return true;
}

std::optional<ApiParam> ApiResolver::ReadParam(const ModuleDraft& module, const Draft& draft, const ParsedParam& param)
{
    const HeaderSource& header = HeaderOf(module, draft);
    DeclarationProblem problem;
    std::optional<ApiParamShape> shape = ApiKindResolver::ResolveParameter(param.Type, LookupFrom(draft.Scope), problem);
    if (!shape)
    {
        Refuse(header.File, draft.Line, "parameter `" + param.Name + "`: " + problem.Problem, problem.Fix);
        return std::nullopt;
    }
    ApiParam result;
    result.Name = param.Name;
    result.Type = shape->Type;
    result.Out = shape->Out;
    if (!param.Default)
        return result;
    if (result.Out)
    {
        Refuse(header.File, draft.Line, "the out parameter `" + param.Name + "` has a default", "remove the default");
        return std::nullopt;
    }
    const std::string value = Trim(*param.Default);
    if (result.Type.Kind == ApiKind::Record)
    {
        if (value != "{}")
        {
            Refuse(header.File, draft.Line, "the record parameter `" + param.Name + "` defaults to `" + value + "`",
                   "default a record parameter to {}: it takes the record's own defaults");
            return std::nullopt;
        }
        result.Default = ApiDefault{"{}", std::nullopt};
        return result;
    }
    if (!IsScalarKind(result.Type.Kind))
    {
        Refuse(header.File, draft.Line, "the parameter `" + param.Name + "` of kind " + std::string(ApiKindName(result.Type.Kind)) + " cannot have a default",
               "a default is a scalar literal, an enumerator, {} on a record, or a constant of this header");
        return std::nullopt;
    }
    std::optional<ApiDefault> parsed = ReadScalarDefault(module, header, value, result.Type, problem);
    if (!parsed)
    {
        Refuse(header.File, draft.Line, "parameter `" + param.Name + "`: " + problem.Problem, problem.Fix);
        return std::nullopt;
    }
    result.Default = std::move(parsed);
    return result;
}

std::optional<ApiHandle> ApiResolver::ReadHandle(const ModuleDraft& module, const Draft& draft)
{
    const HeaderSource& header = HeaderOf(module, draft);
    const std::string& target = draft.Alias.Target;
    const std::size_t angle = target.find('<');
    const std::string base = Trim(angle == std::string::npos ? std::string_view(target) : std::string_view(target).substr(0, angle));
    const bool strong = angle != std::string::npos && LastScopeSegment(base) == "StrongHandle";
    const bool generational = angle == std::string::npos && ApiKindResolver::NamesQualified(base, "GenerationalVector::Handle") &&
                              base.find("GenerationalVector") != std::string::npos;
    if (!strong && !generational)
    {
        Refuse(header.File, draft.Line, "`" + target + "` is not a generational handle: a script handle is never an address or a plain integer",
               "alias a 64-bit generational handle: `using " + draft.Name + " = StrongHandle<" + draft.Name + "Tag>;`");
        return std::nullopt;
    }
    return ApiHandle{draft.Name, draft.CppName, SourceOf(header, draft.Line), draft.Doc, 64};
}

// The payload record of a `ScriptCallback<Payload>` parameter type.
std::optional<std::string> ApiResolver::CallbackPayload(std::string_view typeText, std::string_view scope)
{
    const std::string bare = BareType(typeText);
    const std::size_t open = bare.find('<');
    const std::size_t close = bare.rfind('>');
    if (open == std::string::npos || close == std::string::npos || close < open ||
        LastScopeSegment(Trim(std::string_view(bare).substr(0, open))) != "ScriptCallback")
        return std::nullopt;
    DeclarationProblem ignored;
    const std::optional<ApiType> payload =
        ApiKindResolver::ResolveValue(std::string_view(bare).substr(open + 1, close - open - 1), LookupFrom(scope), ignored);
    if (!payload || payload->Kind != ApiKind::Record)
        return std::nullopt;
    return payload->TypeId;
}

void ApiResolver::ReadEvents(const ModuleDraft& module, std::vector<ApiEvent>& outEvents)
{
    std::map<std::string, const Draft*> unsubscribes;
    for (const Draft& draft : module.Drafts)
        if (draft.Kind == DraftKind::Unsubscribe)
        {
            const HeaderSource& header = HeaderOf(module, draft);
            std::vector<ParsedParam> params = draft.Function.Params;
            std::optional<std::string> context;
            if (!ReadContext(header, draft, params, context))
                continue;
            if (BareType(draft.Function.ReturnType) != "void" || params.size() != 1 ||
                LastScopeSegment(BareType(params[0].Type)) != "SubscriptionId")
            {
                Refuse(header.File, draft.Line, "an Unsubscribe function takes the SubscriptionId alone and returns void",
                       "declare `void " + draft.Name + "(SubscriptionId subscription)`");
                continue;
            }
            unsubscribes[draft.Name.substr(11)] = &draft;
        }

    std::set<std::string> paired;
    for (const Draft& draft : module.Drafts)
    {
        if (draft.Kind != DraftKind::Subscribe)
            continue;
        const HeaderSource& header = HeaderOf(module, draft);
        const std::string name = draft.Name.substr(9);
        std::vector<ParsedParam> params = draft.Function.Params;
        ApiEvent event;
        event.Name = name;
        event.Source = SourceOf(header, draft.Line);
        event.Doc = draft.Doc;
        event.Delivery = *draft.Delivery;
        event.SubscribeCpp = draft.CppName;
        if (!ReadContext(header, draft, params, event.Context))
            continue;
        DeclarationProblem problem;
        const std::optional<ApiParamShape> scope =
            params.size() == 2 ? ApiKindResolver::ResolveParameter(params[0].Type, LookupFrom(draft.Scope), problem) : std::nullopt;
        const std::optional<std::string> payload = params.size() == 2 ? CallbackPayload(params[1].Type, draft.Scope) : std::nullopt;
        if (LastScopeSegment(BareType(draft.Function.ReturnType)) != "SubscriptionId" || !scope || scope->Out ||
            (scope->Type.Kind != ApiKind::Entity && scope->Type.Kind != ApiKind::Handle) || !payload)
        {
            Refuse(header.File, draft.Line,
                   "a Subscribe function takes the scope (an entity or a handle) and a ScriptCallback of a marked record, "
                   "and returns the SubscriptionId",
                   "declare `SubscriptionId " + draft.Name + "(ECS::EntityHandle scope, ScriptCallback<Payload> callback)`");
            continue;
        }
        event.Scope = scope->Type;
        event.PayloadId = *payload;
        const auto partner = unsubscribes.find(name);
        if (partner == unsubscribes.end())
        {
            Refuse(header.File, draft.Line, "the event `" + name + "` has no Unsubscribe" + name,
                   "declare `void Unsubscribe" + name + "(SubscriptionId subscription)` marked // @ge-script-event");
            continue;
        }
        event.UnsubscribeCpp = partner->second->CppName;
        paired.insert(name);
        outEvents.push_back(std::move(event));
    }
    for (const auto& [name, draft] : unsubscribes)
        if (paired.count(name) == 0 &&
            std::none_of(module.Drafts.begin(), module.Drafts.end(),
                         [&](const Draft& d)
                         { return d.Kind == DraftKind::Subscribe && d.Name.substr(9) == name; }))
            Refuse(HeaderOf(module, *draft).File, draft->Line, "Unsubscribe" + name + " has no Subscribe" + name,
                   "declare the Subscribe" + name + " it ends, marked // @ge-script-event <delivery>");
}

std::optional<ApiModule> ApiResolver::ResolveModule(const ModuleDraft& module)
{
    ApiModule result;
    result.Name = module.Name;
    // The first declaration of each marked function, by its full name: a second is an overload.
    std::map<std::string, const Draft*> callables;
    for (const Draft& draft : module.Drafts)
    {
        if (draft.Kind == DraftKind::Function || draft.Kind == DraftKind::Subscribe || draft.Kind == DraftKind::Unsubscribe)
        {
            if (const auto [it, inserted] = callables.emplace(draft.CppName, &draft); !inserted)
            {
                const HeaderSource& first = HeaderOf(module, *it->second);
                const HeaderSource& header = HeaderOf(module, draft);
                Refuse(header.File, draft.Line,
                       "`" + draft.Name + "` overloads the marked function of " + (first.File == header.File ? "" : first.Include + " ") +
                           "line " + std::to_string(it->second->Line),
                       "give each function its own name");
                continue;
            }
        }
        const std::string id = module.Name + "." + draft.Name;
        switch (draft.Kind)
        {
        case DraftKind::Function:
            if (std::optional<ApiFunction> fn = ReadFunction(module, draft))
                result.Functions.push_back(std::move(*fn));
            break;
        case DraftKind::Record:
            if (std::optional<ApiRecord> record = ResolveRecord(id))
                result.Records.push_back(std::move(*record));
            break;
        case DraftKind::Enum:
            if (std::optional<ApiEnum> e = ResolveEnum(id))
                result.Enums.push_back(std::move(*e));
            break;
        case DraftKind::Handle:
            if (std::optional<ApiHandle> handle = ReadHandle(module, draft))
                result.Handles.push_back(std::move(*handle));
            break;
        case DraftKind::Subscribe:
        case DraftKind::Unsubscribe:
            break;
        }
    }
    ReadEvents(module, result.Events);
    for (auto& [id, constant] : m_ReferencedConstants[module.Name])
        result.Constants.push_back(constant);

    const auto byName = [](const auto& a, const auto& b)
    { return a.Name < b.Name; };
    std::sort(result.Constants.begin(), result.Constants.end(), byName);
    std::sort(result.Enums.begin(), result.Enums.end(), byName);
    std::sort(result.Events.begin(), result.Events.end(), byName);
    std::sort(result.Functions.begin(), result.Functions.end(), byName);
    std::sort(result.Handles.begin(), result.Handles.end(), byName);
    std::sort(result.Records.begin(), result.Records.end(), byName);
    if (module.Drafts.empty())
        return std::nullopt;
    return result;
}

} // namespace GameEngine::ComponentScanner
