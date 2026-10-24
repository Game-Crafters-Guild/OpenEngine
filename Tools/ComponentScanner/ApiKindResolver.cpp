#include "ApiKindResolver.h"

#include "Lexical.h"

#include <utility>

namespace GameEngine::ComponentScanner
{

namespace ApiKindResolver
{
namespace
{

using namespace Lexical;

// The kind table's C++ spellings: each type's full name and its kind.
constexpr std::pair<std::string_view, ApiKind> kBuiltIns[] = {
    {"bool", ApiKind::Bool},
    {"int", ApiKind::Int32},
    {"float", ApiKind::Float},
    {"double", ApiKind::Double},
    {"std::int8_t", ApiKind::Int8},
    {"std::int16_t", ApiKind::Int16},
    {"std::int32_t", ApiKind::Int32},
    {"std::int64_t", ApiKind::Int64},
    {"std::uint8_t", ApiKind::UInt8},
    {"std::uint16_t", ApiKind::UInt16},
    {"std::uint32_t", ApiKind::UInt32},
    {"std::uint64_t", ApiKind::UInt64},
    {"GameEngine::int8", ApiKind::Int8},
    {"GameEngine::int16", ApiKind::Int16},
    {"GameEngine::int32", ApiKind::Int32},
    {"GameEngine::int64", ApiKind::Int64},
    {"GameEngine::uint8", ApiKind::UInt8},
    {"GameEngine::uint16", ApiKind::UInt16},
    {"GameEngine::uint32", ApiKind::UInt32},
    {"GameEngine::uint64", ApiKind::UInt64},
    {"GameEngine::float32", ApiKind::Float},
    {"GameEngine::float64", ApiKind::Double},
    {"GameEngine::Mathematics::Radians", ApiKind::Angle},
    {"GameEngine::Mathematics::Vector2", ApiKind::Vec2},
    {"GameEngine::Mathematics::Vector3", ApiKind::Vec3},
    {"GameEngine::Mathematics::Vector4", ApiKind::Vec4},
    {"GameEngine::Mathematics::Quaternion", ApiKind::Quat},
    {"GameEngine::Mathematics::Matrix4x4", ApiKind::Mat4},
    {"GameEngine::ColorLinear", ApiKind::ColorLinear},
    {"GameEngine::ECS::EntityHandle", ApiKind::Entity},
    {"GameEngine::Rendering::RenderLayerMask", ApiKind::RenderLayerMask},
    {"GameEngine::StringId", ApiKind::StringId},
    {"std::string_view", ApiKind::String},
};

// Spellings that are close to a kind but not one, with the fix.
constexpr std::pair<std::string_view, std::string_view> kNearMisses[] = {
    {"char", "use a fixed-width integer (std::int8_t), or std::string_view for text"},
    {"short", "use a fixed-width integer (std::int16_t)"},
    {"long", "use a fixed-width integer (std::int32_t or std::int64_t)"},
    {"unsigned", "use a fixed-width integer (std::uint32_t)"},
    {"signed", "use a fixed-width integer (std::int32_t)"},
    {"size_t", "use a fixed-width integer (std::uint32_t or std::uint64_t)"},
    {"ptrdiff_t", "use a fixed-width integer (std::int64_t)"},
    {"intptr_t", "an address never crosses; take a handle"},
    {"uintptr_t", "an address never crosses; take a handle"},
    {"string", "take std::string_view"},
    {"ColorSRGB", "mark a ColorLinear and convert in the function"},
    {"ColorSrgb", "mark a ColorLinear and convert in the function"},
};

std::string CollapseSpaces(std::string_view text)
{
    std::string result;
    bool space = false;
    for (char c : TrimView(text))
    {
        if (IsWhiteSpace(c))
        {
            space = true;
            continue;
        }
        if (space && !result.empty())
        {
            const char prev = result.back();
            // Keep a space only between two identifier characters ("unsigned int").
            if (IsIdentifierChar(prev) && IsIdentifierChar(c))
                result.push_back(' ');
        }
        space = false;
        result.push_back(c);
    }
    return result;
}

std::vector<std::string_view> Segments(std::string_view name)
{
    std::vector<std::string_view> segments;
    if (StartsWith(name, "::"))
        name.remove_prefix(2);
    std::size_t start = 0;
    while (true)
    {
        const std::size_t sep = name.find("::", start);
        segments.push_back(name.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start));
        if (sep == std::string_view::npos)
            break;
        start = sep + 2;
    }
    return segments;
}

DeclarationProblem Problem(std::string problem, std::string fix)
{
    return DeclarationProblem{std::move(problem), std::move(fix)};
}

// "std::span<const T>" or "std::span<T>" with no extent. A span of T is one the function writes: it is
// resolved only where `outWritable` is given (a parameter), and *outWritable says which it is.
std::optional<ApiType> ResolveSpan(std::string_view text, const ApiLookup& lookup, DeclarationProblem& outProblem,
                                   bool* outWritable)
{
    const std::size_t open = text.find('<');
    const std::size_t close = text.rfind('>');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
    {
        outProblem = Problem("`" + std::string(text) + "` has no element type", "take std::span<const T>");
        return std::nullopt;
    }
    std::string_view element = TrimView(text.substr(open + 1, close - open - 1));
    if (element.find(',') != std::string_view::npos)
    {
        outProblem = Problem("a span with a fixed extent cannot be bound", "take std::span<const T>, whose count travels with it");
        return std::nullopt;
    }
    const bool writable = !StartsWith(element, "const ") && !StartsWith(element, "const");
    if (writable && outWritable == nullptr)
    {
        outProblem = Problem("a span the function writes (`" + std::string(text) + "`) is a parameter only",
                             "take std::span<T> as a parameter and return the number of results");
        return std::nullopt;
    }
    if (outWritable != nullptr)
        *outWritable = writable;
    if (!writable)
        element = TrimView(element.substr(5));
    std::optional<ApiType> elementType = ResolveValue(element, lookup, outProblem);
    if (!elementType)
        return std::nullopt;
    if (elementType->Kind == ApiKind::String || elementType->Kind == ApiKind::Span)
    {
        outProblem = Problem("a span of text or of spans cannot be bound", "take a span of a value kind (numbers, vectors, records)");
        return std::nullopt;
    }
    ApiType span;
    span.Kind = ApiKind::Span;
    span.Element.push_back(std::move(*elementType));
    return span;
}

// The marked type `name` names, looked up as C++ does from `lookup.Scope`: of the marked types
// whose full name ends in the written one, the one in the innermost namespace around the
// declaration wins; one match anywhere else is taken; two or more are refused.
std::optional<ApiType> ResolveMarked(std::string_view name, const ApiLookup& lookup, DeclarationProblem& outProblem)
{
    std::vector<const ApiNamedType*> matches;
    for (const ApiNamedType& type : lookup.Named)
        if (NamesQualified(name, type.CppName))
            matches.push_back(&type);
    if (matches.empty())
        return std::nullopt;

    const std::vector<std::string_view> scope = Segments(lookup.Scope);
    const std::size_t written = Segments(name).size();
    const ApiNamedType* best = nullptr;
    std::size_t bestDepth = 0;
    for (const ApiNamedType* type : matches)
    {
        // The namespaces the written name is looked up in: the full name less the written part.
        const std::vector<std::string_view> full = Segments(type->CppName);
        const std::size_t depth = full.size() - written;
        const bool encloses = depth <= scope.size() && std::equal(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(depth), scope.begin());
        if (encloses && (best == nullptr || depth > bestDepth))
        {
            best = type;
            bestDepth = depth;
        }
    }
    if (best == nullptr && matches.size() == 1)
        best = matches.front();
    if (best != nullptr)
        return ApiType{best->Kind, best->Id, {}};

    std::string candidates;
    std::string spellings;
    for (std::size_t i = 0; i < matches.size(); ++i)
    {
        const char* separator = i == 0 ? "" : i + 1 == matches.size() ? (matches.size() == 2 ? " and " : ", and ") : ", ";
        candidates += separator + matches[i]->Id;
        spellings += std::string(i == 0 ? "" : i + 1 == matches.size() ? " or " : ", ") + "`" + matches[i]->CppName + "`";
    }
    outProblem = Problem("`" + std::string(name) + "` names " + candidates,
                         "qualify the name (" + spellings + "), or rename one of the types");
    return std::nullopt;
}

// ResolveValue, and for a parameter (`outWritableSpan` given) also a span the function writes.
std::optional<ApiType> ResolveValueOrSpan(std::string_view typeText, const ApiLookup& lookup, DeclarationProblem& outProblem,
                                          bool* outWritableSpan)
{
    const std::string text = CollapseSpaces(typeText);
    if (text == "void")
        return ApiType{ApiKind::Void, {}, {}};

    if (text.find('*') != std::string::npos || text.find('&') != std::string::npos)
    {
        outProblem = Problem("`" + text + "` holds an address", "an address never crosses; take a value, a handle or std::span<const T>");
        return std::nullopt;
    }

    const std::size_t angle = text.find('<');
    const std::string base = angle == std::string::npos ? text : text.substr(0, angle);
    if (NamesQualified(base, "std::span") && StartsWith(base, "std::"))
        return ResolveSpan(text, lookup, outProblem, outWritableSpan);

    if (angle != std::string::npos)
    {
        if (StartsWith(base, "std::"))
            outProblem = Problem("`" + text + "` is a standard container, which cannot cross",
                                 "take std::span<const T> for a list, or std::string_view for text");
        else
            outProblem = Problem("`" + text + "` is a template, which cannot be bound", "use a kind of the table or a marked type");
        return std::nullopt;
    }

    for (const auto& [qualified, kind] : kBuiltIns)
        if (NamesQualified(base, qualified))
            return ApiType{kind, {}, {}};

    if (const std::optional<ApiType> marked = ResolveMarked(base, lookup, outProblem))
        return marked;
    if (!outProblem.Problem.empty())
        return std::nullopt;

    for (const auto& [name, fix] : kNearMisses)
        if (LastScopeSegment(base) == name || StartsWith(base, std::string(name) + " "))
        {
            outProblem = Problem("`" + text + "` is not in the kinds", std::string(fix));
            return std::nullopt;
        }

    if (StartsWith(base, "std::"))
    {
        outProblem = Problem("`" + text + "` is a standard library type, which cannot cross",
                             "take std::span<const T> for a list, or std::string_view for text");
        return std::nullopt;
    }
    outProblem = Problem("`" + text + "` is not in the kinds",
                         "use a kind of the table (docs/contributing/script-api-markers.mdx), or mark the type in this "
                         "module's <Module>ScriptApi.h: a struct or an enum class with // @ge-script, a handle alias "
                         "with // @ge-script-handle");
    return std::nullopt;
}

} // namespace

bool NamesQualified(std::string_view spelled, std::string_view qualified)
{
    const std::vector<std::string_view> a = Segments(spelled);
    const std::vector<std::string_view> b = Segments(qualified);
    if (a.size() > b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[a.size() - 1 - i] != b[b.size() - 1 - i])
            return false;
    return true;
}

std::optional<ApiType> ResolveValue(std::string_view typeText, const ApiLookup& lookup,
                                    DeclarationProblem& outProblem)
{
    return ResolveValueOrSpan(typeText, lookup, outProblem, nullptr);
}

std::optional<ApiType> ResolveReturn(std::string_view typeText, const ApiLookup& lookup,
                                     DeclarationProblem& outProblem)
{
    const std::string text = CollapseSpaces(typeText);
    const std::string_view unrooted = StartsWith(text, "::") ? std::string_view(text).substr(2) : std::string_view(text);
    if (NamesQualified(unrooted, "std::string") && StartsWith(unrooted, "std::"))
        return ApiType{ApiKind::String, {}, {}};
    std::optional<ApiType> type = ResolveValue(text, lookup, outProblem);
    if (type && type->Kind == ApiKind::String)
    {
        outProblem = Problem("a returned `" + text + "` can outlive the text it views", "return std::string");
        return std::nullopt;
    }
    if (type && type->Kind == ApiKind::Span)
    {
        outProblem = Problem("a span is a parameter only", "return a value kind, or write the result through one out parameter");
        return std::nullopt;
    }
    return type;
}

std::optional<ApiParamShape> ResolveParameter(std::string_view typeText, const ApiLookup& lookup,
                                              DeclarationProblem& outProblem)
{
    std::string text = CollapseSpaces(typeText);
    if (text.find("volatile") != std::string::npos)
    {
        outProblem = Problem("a volatile parameter cannot be bound", "drop volatile");
        return std::nullopt;
    }
    if (EndsWith(text, "&&"))
    {
        outProblem = Problem("an rvalue reference parameter cannot be bound", "take the value, or const T&");
        return std::nullopt;
    }

    bool reference = false;
    std::size_t pointers = 0;
    while (!text.empty() && (text.back() == '&' || text.back() == '*'))
    {
        if (text.back() == '&')
            reference = true;
        else
            ++pointers;
        text.pop_back();
        text = Trim(text);
    }

    bool isConst = false;
    if (StartsWith(text, "const ") || text == "const")
    {
        isConst = true;
        text = Trim(std::string_view(text).substr(5));
    }
    if (EndsWith(text, " const") || EndsWith(text, ">const"))
    {
        isConst = true;
        text = Trim(std::string_view(text).substr(0, text.size() - 5));
    }

    if (pointers > 1 || (pointers == 1 && reference))
    {
        outProblem = Problem("`" + CollapseSpaces(typeText) + "` holds an address", "an address never crosses; take a value or a handle");
        return std::nullopt;
    }
    if (pointers == 1)
    {
        outProblem = Problem("`" + CollapseSpaces(typeText) + "` is an address",
                             isConst ? "take const T& for one value, std::span<const T> for many"
                                     : "take T& for the value the function writes");
        return std::nullopt;
    }

    bool writableSpan = false;
    std::optional<ApiType> type = ResolveValueOrSpan(text, lookup, outProblem, &writableSpan);
    if (!type)
        return std::nullopt;
    if (type->Kind == ApiKind::Void)
    {
        outProblem = Problem("a void parameter cannot be bound", "remove it");
        return std::nullopt;
    }
    if (type->Kind == ApiKind::Span && reference)
    {
        outProblem = Problem("a span by reference cannot be bound",
                             "take std::span<const T> by value for a list in, std::span<T> by value for results");
        return std::nullopt;
    }

    ApiParamShape shape;
    shape.Out = (reference && !isConst) || writableSpan;
    if (shape.Out && type->Kind == ApiKind::String)
    {
        outProblem = Problem("text cannot be an out parameter", "take std::string_view in, or return std::string");
        return std::nullopt;
    }
    shape.Type = std::move(*type);
    return shape;
}

std::uint32_t SizeOf(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Bool:
    case ApiKind::Int8:
    case ApiKind::UInt8: return 1;
    case ApiKind::Int16:
    case ApiKind::UInt16: return 2;
    case ApiKind::Int32:
    case ApiKind::UInt32:
    case ApiKind::Float:
    case ApiKind::Angle:
    case ApiKind::Entity:
    case ApiKind::RenderLayerMask: return 4;
    case ApiKind::Int64:
    case ApiKind::UInt64:
    case ApiKind::Double:
    case ApiKind::StringId:
    case ApiKind::Handle:
    case ApiKind::Vec2: return 8;
    case ApiKind::Vec3: return 12;
    case ApiKind::Vec4:
    case ApiKind::Quat:
    case ApiKind::ColorLinear: return 16;
    case ApiKind::Mat4: return 64;
    default: return 0;
    }
}

std::uint32_t AlignOf(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Vec2:
    case ApiKind::Vec3:
    case ApiKind::Vec4:
    case ApiKind::Quat:
    case ApiKind::Mat4:
    case ApiKind::ColorLinear: return 4;
    default: return SizeOf(kind);
    }
}

bool IsInteger(ApiKind kind)
{
    switch (kind)
    {
    case ApiKind::Int8:
    case ApiKind::Int16:
    case ApiKind::Int32:
    case ApiKind::Int64:
    case ApiKind::UInt8:
    case ApiKind::UInt16:
    case ApiKind::UInt32:
    case ApiKind::UInt64: return true;
    default: return false;
    }
}

} // namespace ApiKindResolver

} // namespace GameEngine::ComponentScanner
