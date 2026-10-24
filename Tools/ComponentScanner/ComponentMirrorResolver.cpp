#include "ComponentMirrorResolver.h"

#include "ApiDraft.h"
#include "ApiKindResolver.h"
#include "ApiMarkerLines.h"
#include "CommentStripper.h"
#include "EnumScanner.h"
#include "FunctionParser.h"
#include "IncludeGraph.h"
#include "Lexical.h"
#include "ScopeLookup.h"
#include "SourceFiles.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace GameEngine::ComponentScanner::ComponentMirrorResolver
{
namespace
{

using namespace Lexical;
using namespace ScopeLookup;
namespace fs = std::filesystem;

constexpr int kMaxAliasDepth = 8;
constexpr int kMaxConstantDepth = 32;
// The most bytes a mirrored field holds: C#'s StructLayout.Size and FieldOffset are int.
constexpr std::int64_t kMaxFieldBytes = std::numeric_limits<std::int32_t>::max();
// The value of an extent sum that leaves the int64 range.
constexpr std::int64_t kSaturated = std::numeric_limits<std::int64_t>::max();

// The layouts of the hand-written kinds of Scripting.Bindings: a GUID is 16 bytes, Math::Curve is
// CurveBase<32> (a key count, 3 bytes of padding and 32 keys of 20 bytes), a BoundingBox is two
// Vector3. The layout pins prove them for every field that holds one.
constexpr std::uint32_t kAssetGuidSize = 16;
constexpr std::uint32_t kCurveSize = 644;
constexpr std::uint32_t kCurveAlign = 4;
constexpr std::uint32_t kBoundingBoxSize = 24;
constexpr std::uint32_t kBoundingBoxAlign = 4;

constexpr std::string_view kNoKindFix =
    "give it a type a mirror holds (a scalar, a Mathematics type, ColorLinear, ECS::EntityHandle, StringId, "
    "Rendering::RenderLayerMask, an AssetRef, GUID, Math::Curve, Mathematics::BoundingBox, an enum, a reflected "
    "struct, or a fixed array of one of them; a char array for text), or mark the component // @ge-engine-internal "
    "when it is engine runtime state";

// The types a generated mirror would collide with or hide: the hand-written types of
// GameEngine.Scripting in Scripting.Bindings and ECS.ABI, GameEngine.Entity, and the
// System.Numerics types a mirror's field takes. A mirror of one of these names fails the C#
// build, or silently takes the place of the field type in the generated files.
constexpr std::string_view kHandWrittenTypes[] = {
    "AssetGuid",
    "BindingErrors",
    "BindingTable",
    "BoundingBox",
    "BuiltInComponent",
    "BuiltInComponentAttribute",
    "ColorLinear",
    "ComponentText",
    "Curve",
    "CurveInterp",
    "CurveKey",
    "CurveTangentMode",
    "EngineCallException",
    "Entity",
    "IComponent",
    "Matrix4x4",
    "Quaternion",
    "RenderLayerMask",
    "ScriptCallbackData",
    "ScriptSubscriptionRegistry",
    "ScriptText",
    "StringId",
    "Vector2",
    "Vector3",
    "Vector4",
};

// One enum declaration of the mirror's table.
struct EnumEntry
{
    std::string CppName;
    std::string Scope;
    EnumDecl Decl;
    std::string Header;
    std::string File;
    std::string Doc;
};

// One constexpr constant of a component header.
struct ConstantEntry
{
    std::string CppName;
    std::string Scope;
    std::string Literal;
    std::size_t Source = 0;
};

// One value of a field's type, as the mirror holds it.
struct FieldType
{
    MirrorKind Kind = MirrorKind::Value;
    ApiKind Value = ApiKind::Void;
    std::string TypeName;
    std::uint32_t Size = 0;
    std::uint32_t Align = 1;
};

// True when a + b leaves the int64 range.
bool AddOverflows(std::int64_t a, std::int64_t b)
{
    return (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
           (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b);
}

// An integer literal with an optional u/U/l/L suffix, decimal or hexadecimal.
std::optional<std::int64_t> IntegerLiteral(std::string_view text)
{
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == 'u' || text[end - 1] == 'U' || text[end - 1] == 'l' || text[end - 1] == 'L'))
        --end;
    std::string_view digits = text.substr(0, end);
    int base = 10;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        digits.remove_prefix(2);
        base = 16;
    }
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (digits.empty() || ec != std::errc{} || ptr != digits.data() + digits.size())
        return std::nullopt;
    return value;
}

class Resolver
{
  public:
    Resolver(const std::vector<ComponentHeader>& headers, const std::vector<ReflectedStruct>& reflected,
             const std::map<std::string, std::string>& aliases, std::vector<ApiRefusal>& refusals)
        : m_Headers(headers), m_Reflected(reflected), m_Aliases(aliases), m_Refusals(refusals)
    {
    }

    ComponentMirrors Run(const fs::path& modulesDir, const ApiDescription& description)
    {
        ReadComponentHeaders();
        ReadIncludedEnums(modulesDir);
        Classify();
        for (std::size_t i = 0; i < m_Reflected.size(); ++i)
            if (m_Mirrors.Structs[i].Class == ComponentClass::GameData || m_Mirrors.Structs[i].Class == ComponentClass::EngineOwned)
                ResolveStruct(i);
        for (std::size_t i = 0; i < m_Reflected.size(); ++i)
            if (m_Element[i] && m_Mirrors.Structs[i].Class == ComponentClass::GameData)
                m_Mirrors.Structs[i].Class = ComponentClass::Element;
        CollectEnums(description);
        RefuseNameCollisions(description);
        return std::move(m_Mirrors);
    }

  private:
    enum class State : std::uint8_t
    {
        Unvisited,
        Resolving,
        Resolved,
        Refused,
    };

    enum class EnumCheck : std::uint8_t
    {
        Unchecked,
        Usable,
        Refused,
    };

    const std::vector<ComponentHeader>& m_Headers;
    const std::vector<ReflectedStruct>& m_Reflected;
    const std::map<std::string, std::string>& m_Aliases;
    std::vector<ApiRefusal>& m_Refusals;

    ComponentMirrors m_Mirrors;
    std::vector<State> m_States;
    std::vector<bool> m_Element;
    std::vector<std::string> m_StructNames;
    std::vector<EnumEntry> m_Enums;
    std::vector<std::string> m_EnumNames;
    std::set<std::string> m_EnumSimpleNames;
    std::vector<EnumCheck> m_EnumChecks;
    std::vector<ApiKind> m_EnumUnderlying;
    std::set<std::size_t> m_UsedEnums;
    std::vector<ConstantEntry> m_Constants;

    void Refuse(const std::string& file, int line, std::string problem, std::string fix)
    {
        m_Refusals.push_back(ApiRefusal{file, line, std::move(problem), std::move(fix)});
    }

    void AddEnums(const std::string& stripped, const std::vector<std::string>& lines, const std::string& header,
                  const std::string& file)
    {
        for (ScopedEnumDecl& scoped : EnumScanner::ScanScoped(stripped))
        {
            EnumEntry entry;
            entry.CppName = std::move(scoped.CppName);
            entry.Scope = std::move(scoped.Scope);
            entry.Header = header;
            entry.File = file;
            entry.Doc = ApiMarkerLines::DocAbove(lines, scoped.Decl.DeclLine);
            entry.Decl = std::move(scoped.Decl);
            m_EnumNames.push_back(entry.CppName);
            m_EnumSimpleNames.insert(entry.Decl.Name);
            m_Enums.push_back(std::move(entry));
        }
    }

    void ReadComponentHeaders()
    {
        for (std::size_t i = 0; i < m_Headers.size(); ++i)
        {
            const ComponentHeader& header = m_Headers[i];
            AddEnums(header.Stripped, header.Lines, header.IncludePath, header.File);
            for (ParsedConstant& constant : FunctionParser::ScanConstants(header.Stripped))
                m_Constants.push_back(
                    ConstantEntry{Qualify(constant.Scope, constant.Name), ScopeName(constant.Scope), std::move(constant.Literal), i});
        }
    }

    // The enums of the headers the component headers include, followed through the Include trees
    // of the modules (IncludeGraph). A component header reached that way is read once, as a
    // component header. The headers are read in include-path order, so the table does not
    // depend on which component header reached a header first.
    void ReadIncludedEnums(const fs::path& modulesDir)
    {
        std::vector<fs::path> includeRoots;
        std::error_code ec;
        for (fs::directory_iterator it(modulesDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (const fs::path include = it->path() / "Include"; fs::is_directory(include, ec))
                includeRoots.push_back(include);
        std::sort(includeRoots.begin(), includeRoots.end());

        std::set<std::string> componentFiles;
        for (const ComponentHeader& header : m_Headers)
            componentFiles.insert(Utf8(FullPath(PathFromUtf8(header.File))));
        IncludeGraph graph(std::move(includeRoots));
        std::map<std::string, const IncludedHeader*> included;
        for (const ComponentHeader& header : m_Headers)
            for (const IncludedHeader* reached : graph.Closure(PathFromUtf8(header.File), header.Lines))
                if (componentFiles.count(Utf8(reached->File)) == 0)
                    included.emplace(reached->IncludePath, reached);
        for (const auto& [includePath, reached] : included)
            AddEnums(reached->Stripped, reached->Lines, includePath, Utf8(reached->File));
    }

    void Classify()
    {
        m_States.assign(m_Reflected.size(), State::Unvisited);
        m_Element.assign(m_Reflected.size(), false);
        m_EnumChecks.assign(m_Enums.size(), EnumCheck::Unchecked);
        m_EnumUnderlying.assign(m_Enums.size(), ApiKind::Int32);
        for (const ReflectedStruct& reflected : m_Reflected)
        {
            const ComponentHeader& header = m_Headers[reflected.Source];
            MirrorStruct mirror;
            mirror.Name = reflected.Name;
            mirror.CppName = QualifiedName(reflected.Namespace, reflected.Name);
            mirror.Header = reflected.Header;
            mirror.File = header.File;
            mirror.Line = reflected.Line;
            mirror.Doc = ApiMarkerLines::DocAbove(header.Lines, reflected.Line);
            mirror.Class = reflected.EditorOnly       ? ComponentClass::EditorOnly
                           : reflected.EngineInternal ? ComponentClass::EngineInternal
                           : reflected.ReadOnly       ? ComponentClass::EngineOwned
                                                      : ComponentClass::GameData;
            m_StructNames.push_back(mirror.CppName);
            m_Mirrors.Structs.push_back(std::move(mirror));
        }
    }

    // The value of an extent or a constant: + and - over integer literals and constants. A constant
    // is looked up in the struct `owner`, then in header `source`, then in every component header.
    std::optional<std::int64_t> Evaluate(std::string_view text, std::string_view scope, std::size_t source,
                                         std::string_view owner, int depth)
    {
        if (depth > kMaxConstantDepth)
            return std::nullopt;
        text = TrimView(text);
        if (StartsWith(text, "{") && EndsWith(text, "}"))
            text = TrimView(text.substr(1, text.size() - 2));
        std::int64_t total = 0;
        std::int64_t sign = 1;
        bool expectTerm = true;
        std::size_t i = 0;
        while (i < text.size())
        {
            const char c = text[i];
            if (IsWhiteSpace(c))
            {
                ++i;
                continue;
            }
            if (!expectTerm)
            {
                if (c != '+' && c != '-')
                    return std::nullopt;
                sign = c == '+' ? 1 : -1;
                expectTerm = true;
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < text.size() && (IsIdentifierChar(text[end]) || text[end] == ':'))
                ++end;
            if (end == i)
                return std::nullopt;
            const std::string_view term = text.substr(i, end - i);
            const std::optional<std::int64_t> value =
                IsAsciiDigit(term[0]) ? IntegerLiteral(term) : ConstantValue(term, scope, source, owner, depth);
            if (!value)
                return std::nullopt;
            // A sum past the int64 range, here or in a constant it reads, evaluates to kSaturated,
            // which the extent check refuses as past the cap.
            if (*value >= kSaturated || *value <= -kSaturated || AddOverflows(total, sign * *value))
                return kSaturated;
            total += sign * *value;
            expectTerm = false;
            i = end;
        }
        if (expectTerm)
            return std::nullopt;
        return total;
    }

    std::optional<std::int64_t> ConstantValue(std::string_view written, std::string_view scope, std::size_t source,
                                              std::string_view owner, int depth)
    {
        const std::string ownerPrefix = std::string(owner) + "::";
        for (int tier = 0; tier < 3; ++tier)
        {
            std::vector<std::string> names;
            std::vector<std::size_t> entries;
            for (std::size_t i = 0; i < m_Constants.size(); ++i)
            {
                const ConstantEntry& constant = m_Constants[i];
                const bool inTier = tier == 0   ? StartsWith(constant.CppName, ownerPrefix)
                                    : tier == 1 ? constant.Source == source
                                                : true;
                if (!inTier)
                    continue;
                names.push_back(constant.CppName);
                entries.push_back(i);
            }
            const std::size_t found = LookUp(written, scope, names);
            if (found == kAmbiguous)
                return std::nullopt;
            if (found == kNotFound)
                continue;
            const ConstantEntry& constant = m_Constants[entries[found]];
            return Evaluate(constant.Literal, constant.Scope, constant.Source, constant.Scope, depth + 1);
        }
        return std::nullopt;
    }

    // True when the enum has an explicit integer underlying type; refused once otherwise.
    bool CheckEnum(std::size_t index)
    {
        if (m_EnumChecks[index] != EnumCheck::Unchecked)
            return m_EnumChecks[index] == EnumCheck::Usable;
        const EnumEntry& entry = m_Enums[index];
        std::optional<ApiType> underlying;
        if (entry.Decl.Underlying)
        {
            DeclarationProblem problem;
            const std::vector<ApiNamedType> none;
            underlying = ApiKindResolver::ResolveValue(*entry.Decl.Underlying, ApiLookup{none, entry.Scope}, problem);
        }
        if (!underlying || !ApiKindResolver::IsInteger(underlying->Kind))
        {
            Refuse(entry.File, entry.Decl.DeclLine, "the enum `" + entry.CppName + "` has no explicit underlying type",
                   "write one: `enum class " + entry.Decl.Name + " : std::uint8_t`");
            m_EnumChecks[index] = EnumCheck::Refused;
            return false;
        }
        m_EnumChecks[index] = EnumCheck::Usable;
        m_EnumUnderlying[index] = underlying->Kind;
        m_UsedEnums.insert(index);
        return true;
    }

    // One value of a field's type. nullopt with `refused` set when a refusal is made, and with it
    // clear for a type no kind covers.
    std::optional<FieldType> ResolveType(const MirrorStruct& owner, const ReflectedField& field, bool& refused)
    {
        std::string base = BareType(field.Type);
        for (int depth = 0; depth < kMaxAliasDepth && base.find('<') == std::string::npos; ++depth)
        {
            const auto alias = m_Aliases.find(std::string(LastScopeSegment(base)));
            if (alias == m_Aliases.end())
                break;
            base = BareType(alias->second);
        }
        const std::string head = base.substr(0, base.find('<'));
        if (ApiKindResolver::NamesQualified(head, "GameEngine::Components::AssetRef") ||
            NamesFromScope(head, "GameEngine::GUID", owner.CppName))
            return FieldType{MirrorKind::AssetGuid, ApiKind::Void, {}, kAssetGuidSize, 1};
        if (head != base)
            return std::nullopt;
        if (NamesFromScope(base, "GameEngine::Math::Curve", owner.CppName) ||
            NamesFromScope(base, "GameEngine::Math::Curve32", owner.CppName))
            return FieldType{MirrorKind::Curve, ApiKind::Void, {}, kCurveSize, kCurveAlign};
        if (NamesFromScope(base, "GameEngine::Mathematics::BoundingBox", owner.CppName))
            return FieldType{MirrorKind::BoundingBox, ApiKind::Void, {}, kBoundingBoxSize, kBoundingBoxAlign};

        const std::size_t enumIndex = LookUp(base, owner.CppName, m_EnumNames);
        if (enumIndex == kAmbiguous)
        {
            std::string places;
            for (const EnumEntry& entry : m_Enums)
                if (ApiKindResolver::NamesQualified(base, entry.CppName))
                    places += (places.empty() ? "" : " and ") + entry.File + ":" + std::to_string(entry.Decl.DeclLine);
            Refuse(owner.File, field.Line,
                   "the field `" + field.Name + "` of `" + owner.Name + "` names the enum `" + base + "`, which the mirror's enum "
                   "table holds twice: " + places,
                   "keep one declaration, or qualify the name");
            refused = true;
            return std::nullopt;
        }
        if (enumIndex != kNotFound)
        {
            if (!CheckEnum(enumIndex))
            {
                refused = true;
                return std::nullopt;
            }
            const ApiKind underlying = m_EnumUnderlying[enumIndex];
            return FieldType{MirrorKind::Enum, ApiKind::Void, m_Enums[enumIndex].CppName, ApiKindResolver::SizeOf(underlying),
                             ApiKindResolver::AlignOf(underlying)};
        }

        if (const std::size_t structIndex = LookUp(base, owner.CppName, m_StructNames); structIndex < m_StructNames.size())
            return StructType(owner, field, structIndex, refused);

        DeclarationProblem problem;
        const std::vector<ApiNamedType> none;
        if (const std::optional<ApiType> type = ApiKindResolver::ResolveValue(base, ApiLookup{none, owner.CppName}, problem))
        {
            switch (type->Kind)
            {
            case ApiKind::Void:
            case ApiKind::String:
            case ApiKind::Span:
            case ApiKind::Record:
            case ApiKind::Enum:
            case ApiKind::Handle: return std::nullopt;
            default:
                return FieldType{MirrorKind::Value, type->Kind, {}, ApiKindResolver::SizeOf(type->Kind),
                                 ApiKindResolver::AlignOf(type->Kind)};
            }
        }
        if (m_EnumSimpleNames.count(std::string(LastScopeSegment(base))) != 0)
        {
            Refuse(owner.File, field.Line,
                   "the field `" + field.Name + "` of `" + owner.Name + "` names the enum `" + base +
                       "`, which the mirror's enum table does not hold under that name",
                   "declare the enum in a component header or in a module header the component header includes, and "
                   "write its name as C++ finds it from the component");
            refused = true;
        }
        return std::nullopt;
    }

    std::optional<FieldType> StructType(const MirrorStruct& owner, const ReflectedField& field, std::size_t index, bool& refused)
    {
        const MirrorStruct& target = m_Mirrors.Structs[index];
        if (!HasMirror(target.Class))
        {
            Refuse(owner.File, field.Line,
                   "the field `" + field.Name + "` of `" + owner.Name + "` holds `" + target.Name + "`, which is " +
                       (target.Class == ComponentClass::EditorOnly ? "editor-only" : "engine-internal") + " and has no mirror",
                   "hold the values in fields of `" + owner.Name + "`, or take the marker off `" + target.Name + "`");
            refused = true;
            return std::nullopt;
        }
        if (!ResolveStruct(index))
        {
            refused = true;
            return std::nullopt;
        }
        m_Element[index] = true;
        return FieldType{MirrorKind::Struct, ApiKind::Void, target.CppName, target.Size, target.Align};
    }

    // Resolves one field into `out` and its alignment into `outAlign`; false when the field refuses
    // the component.
    bool ResolveField(const MirrorStruct& owner, const ReflectedStruct& reflected, const ReflectedField& field, MirrorField& out,
                      std::uint32_t& outAlign)
    {
        out.Name = field.Name;
        out.Hidden = field.Hidden;
        out.ReadOnly = field.ReadOnly;
        out.Line = field.Line;
        std::uint32_t extent = 0;
        if (field.ArrayExtent)
        {
            const std::optional<std::int64_t> value = Evaluate(*field.ArrayExtent, owner.CppName, reflected.Source, owner.CppName, 0);
            if (!value || *value <= 0)
            {
                Refuse(owner.File, field.Line,
                       "the extent `" + Trim(*field.ArrayExtent) + "` of the array field `" + field.Name + "` of `" + owner.Name +
                           "` is not an integer literal or a sum of constexpr integer constants",
                       "use a constant in this header");
                return false;
            }
            if (*value > kMaxFieldBytes)
                return RefuseExtentPastTheCap(owner, field);
            extent = static_cast<std::uint32_t>(*value);
        }
        if (extent != 0 && BareType(field.Type) == "char")
        {
            out.Kind = MirrorKind::Text;
            out.Extent = extent;
            out.Size = extent;
            return true;
        }
        bool refused = false;
        const std::optional<FieldType> type = ResolveType(owner, field, refused);
        if (!type)
        {
            if (!refused)
                Refuse(owner.File, field.Line,
                       "the field `" + field.Name + "` of `" + owner.Name + "` has the type `" + BareType(field.Type) +
                           "`, which no mirror kind covers, so the component cannot be mirrored",
                       std::string(kNoKindFix));
            return false;
        }
        if (extent != 0 && type->Kind == MirrorKind::Enum)
        {
            Refuse(owner.File, field.Line,
                   "the array field `" + field.Name + "` of `" + owner.Name +
                       "` holds enums, and no setter can check an element written through the array",
                   "hold the values in an array of the enum's underlying type, or give each element its own field");
            return false;
        }
        if (extent != 0 && static_cast<std::int64_t>(type->Size) * extent > kMaxFieldBytes)
            return RefuseExtentPastTheCap(owner, field);
        out.Kind = type->Kind;
        out.Value = type->Value;
        out.TypeName = type->TypeName;
        out.Extent = extent;
        out.Size = type->Size * (extent == 0 ? 1 : extent);
        outAlign = type->Align;
        return true;
    }

    // Refuses an array field larger than a C# struct holds; returns false.
    bool RefuseExtentPastTheCap(const MirrorStruct& owner, const ReflectedField& field)
    {
        Refuse(owner.File, field.Line,
               "the extent `" + Trim(*field.ArrayExtent) + "` of the array field `" + field.Name + "` of `" + owner.Name +
                   "` makes the field larger than " + std::to_string(kMaxFieldBytes) + " bytes, the most a C# struct holds",
               "hold fewer elements");
        return false;
    }

    // Lays a mirrored struct out as the C++ compiler does; false when the struct is refused.
    bool ResolveStruct(std::size_t index)
    {
        if (m_States[index] == State::Resolved)
            return true;
        if (m_States[index] != State::Unvisited)
            return false;
        m_States[index] = State::Resolving;
        const ReflectedStruct& reflected = m_Reflected[index];
        MirrorStruct& mirror = m_Mirrors.Structs[index];
        bool ok = true;
        std::uint32_t offset = 0;
        std::uint32_t align = 1;
        std::vector<MirrorField> fields;
        for (const ReflectedField& field : reflected.Fields)
        {
            MirrorField resolved;
            std::uint32_t fieldAlign = 1;
            if (!ResolveField(mirror, reflected, field, resolved, fieldAlign))
            {
                ok = false;
                continue;
            }
            offset = (offset + fieldAlign - 1) / fieldAlign * fieldAlign;
            resolved.Offset = offset;
            offset += resolved.Size;
            align = std::max(align, fieldAlign);
            fields.push_back(std::move(resolved));
        }
        ok = RefuseNestedNameCollisions(mirror, fields) && ok;
        ok = RefuseMembersNamedLikeTheStruct(mirror, fields) && ok;
        if (!ok)
        {
            m_States[index] = State::Refused;
            return false;
        }
        mirror.Fields = std::move(fields);
        mirror.Align = align;
        // C++ gives a struct with no fields (a tag) one byte, and so does C#.
        mirror.Size = std::max<std::uint32_t>(1, (offset + align - 1) / align * align);
        m_States[index] = State::Resolved;
        return true;
    }

    // The nested <Field>Array type of an array or text field names no other member.
    bool RefuseNestedNameCollisions(const MirrorStruct& mirror, const std::vector<MirrorField>& fields)
    {
        std::set<std::string> members;
        for (const MirrorField& field : fields)
        {
            members.insert(MirrorMemberName(field.Name));
            members.insert("m_" + MirrorMemberName(field.Name));
        }
        bool ok = true;
        for (const MirrorField& field : fields)
        {
            if (field.Extent == 0 || members.count(MirrorArrayType(field.Name)) == 0)
                continue;
            Refuse(mirror.File, field.Line,
                   "the array field `" + field.Name + "` of `" + mirror.Name + "` needs the nested type `" + MirrorArrayType(field.Name) +
                       "`, which another member of `" + mirror.Name + "` names",
                   "rename the field or the other member");
            ok = false;
        }
        return ok;
    }

    // No member and no nested type takes the name of its struct: C# refuses a member named like
    // its enclosing type (CS0542).
    bool RefuseMembersNamedLikeTheStruct(const MirrorStruct& mirror, const std::vector<MirrorField>& fields)
    {
        bool ok = true;
        for (const MirrorField& field : fields)
        {
            const std::string member = MirrorMemberName(field.Name);
            const std::string nested = MirrorArrayType(field.Name);
            if (member != mirror.Name && (field.Extent == 0 || nested != mirror.Name))
                continue;
            Refuse(mirror.File, field.Line,
                   "the field `" + field.Name + "` of `" + mirror.Name + "` takes the C# name `" + mirror.Name +
                       "`, the name of its struct, which C# refuses for a member",
                   "rename the field");
            ok = false;
        }
        return ok;
    }

    void CollectEnums(const ApiDescription& description)
    {
        for (const std::size_t index : m_UsedEnums)
        {
            const EnumEntry& entry = m_Enums[index];
            MirrorEnum mirror;
            mirror.Name = entry.Decl.Name;
            mirror.CppName = entry.CppName;
            mirror.Header = entry.Header;
            mirror.Doc = entry.Doc;
            mirror.Underlying = m_EnumUnderlying[index];
            for (const EnumMember& member : entry.Decl.Members)
                mirror.Values.push_back(ApiEnumValue{member.Name, member.Value});
            for (const ApiModule& module : description.Modules)
                for (const ApiEnum& bound : module.Enums)
                    if (bound.CppName == entry.CppName)
                    {
                        mirror.Bound = true;
                        mirror.Name = bound.Name;
                    }
            m_Mirrors.Enums.push_back(std::move(mirror));
        }
    }

    // A C# type name is taken once: by a hand-written type the mirrors sit beside, by a module of
    // the description, its records, enums and handles, then by the mirrors and the enums they
    // emit, in scan order.
    void RefuseNameCollisions(const ApiDescription& description)
    {
        std::map<std::string, std::string> taken;
        for (const std::string_view name : kHandWrittenTypes)
            taken.emplace(name, std::string());
        for (const ApiModule& module : description.Modules)
        {
            taken.emplace(module.Name, "the " + module.Name + " script module");
            for (const ApiRecord& record : module.Records)
                taken.emplace(record.Name, record.CppName);
            for (const ApiEnum& e : module.Enums)
                taken.emplace(e.Name, e.CppName);
            for (const ApiHandle& handle : module.Handles)
                taken.emplace(handle.Name, handle.CppName);
        }
        for (const MirrorStruct& mirror : m_Mirrors.Structs)
            if (HasMirror(mirror.Class))
                Take(taken, mirror.Name, mirror.CppName, mirror.File, mirror.Line);
        for (const std::size_t index : m_UsedEnums)
            Take(taken, m_Enums[index].Decl.Name, m_Enums[index].CppName, m_Enums[index].File, m_Enums[index].Decl.DeclLine);
    }

    void Take(std::map<std::string, std::string>& taken, const std::string& name, const std::string& cppName, const std::string& file,
              int line)
    {
        const auto [it, inserted] = taken.emplace(name, cppName);
        if (inserted || it->second == cppName)
            return;
        // A hand-written type is taken with no C++ name.
        const bool handWritten = it->second.empty();
        Refuse(file, line,
               "the C# name `" + name + "` of `" + cppName + "` is the name of " +
                   (handWritten ? "the hand-written type `" + name + "`" : "another generated type, `" + it->second + "`"),
               handWritten ? "rename the C++ type" : "rename one of them");
    }
};

} // namespace

ComponentMirrors Resolve(const std::vector<ComponentHeader>& headers, const std::vector<ReflectedStruct>& reflected,
                         const std::map<std::string, std::string>& aliases, const fs::path& modulesDir,
                         const ApiDescription& description, std::vector<ApiRefusal>& outRefusals)
{
    return Resolver(headers, reflected, aliases, outRefusals).Run(modulesDir, description);
}

} // namespace GameEngine::ComponentScanner::ComponentMirrorResolver
