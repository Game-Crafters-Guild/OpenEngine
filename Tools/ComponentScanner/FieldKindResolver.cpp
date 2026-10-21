#include "FieldKindResolver.h"

#include "Lexical.h"

#include <string_view>
#include <utility>

namespace GameEngine::ComponentScanner::FieldKindResolver
{
namespace
{

constexpr int kMaxAliasDepth = 8;

constexpr std::pair<std::string_view, FieldKind> kKnownTypes[] = {
    {"bool", FieldKind::Bool},
    {"char", FieldKind::Int8},
    {"signed char", FieldKind::Int8},
    {"int8_t", FieldKind::Int8},
    {"int8", FieldKind::Int8},
    {"unsigned char", FieldKind::UInt8},
    {"uint8_t", FieldKind::UInt8},
    {"uint8", FieldKind::UInt8},
    {"byte", FieldKind::UInt8},
    {"short", FieldKind::Int16},
    {"int16_t", FieldKind::Int16},
    {"int16", FieldKind::Int16},
    {"unsigned short", FieldKind::UInt16},
    {"uint16_t", FieldKind::UInt16},
    {"uint16", FieldKind::UInt16},
    {"int", FieldKind::Int32},
    {"int32_t", FieldKind::Int32},
    {"int32", FieldKind::Int32},
    {"unsigned", FieldKind::UInt32},
    {"unsigned int", FieldKind::UInt32},
    {"uint32_t", FieldKind::UInt32},
    {"uint32", FieldKind::UInt32},
    {"long long", FieldKind::Int64},
    {"int64_t", FieldKind::Int64},
    {"int64", FieldKind::Int64},
    {"unsigned long long", FieldKind::UInt64},
    {"uint64_t", FieldKind::UInt64},
    {"uint64", FieldKind::UInt64},
    {"StringId", FieldKind::UInt64},
    {"float", FieldKind::Float},
    {"float32", FieldKind::Float},
    {"double", FieldKind::Double},
    {"float64", FieldKind::Double},
    {"Vector2", FieldKind::Vec2},
    {"Vector3", FieldKind::Vec3},
    {"Vector4", FieldKind::Vec4},
    {"Quaternion", FieldKind::Quat},
    {"ColorLinear", FieldKind::Color},
    {"Color", FieldKind::Color},
    {"AssetRef", FieldKind::AssetGuid},
    {"EntityHandle", FieldKind::EntityHandle},
    {"RenderLayerMask", FieldKind::RenderLayerMask},
};

FieldKind KnownKind(std::string_view name)
{
    for (const auto& [known, kind] : kKnownTypes)
        if (known == name)
            return kind;
    return FieldKind::Unknown;
}

// "const ::GameEngine::Mathematics::Vector3" -> "Vector3"; "std::uint32_t" -> "uint32_t";
// "AssetRef<AssetType::Model>" -> "AssetRef"; "unsigned   int" -> "unsigned int". The
// cv/storage words are replaced by a space, template arguments are cut, whitespace runs
// collapse to one space, and the last "::" segment is kept.
std::string Simplify(std::string_view typeText)
{
    std::string withoutQualifiers;
    std::size_t i = 0;
    while (i < typeText.size())
    {
        if (Lexical::IsIdentifierStart(typeText[i]) && (i == 0 || !Lexical::IsIdentifierChar(typeText[i - 1])))
        {
            std::size_t end = i;
            while (end < typeText.size() && Lexical::IsIdentifierChar(typeText[end]))
                ++end;
            const std::string_view word = typeText.substr(i, end - i);
            if (word == "const" || word == "volatile" || word == "mutable")
                withoutQualifiers.push_back(' ');
            else
                withoutQualifiers.append(word);
            i = end;
            continue;
        }
        withoutQualifiers.push_back(typeText[i]);
        ++i;
    }

    std::string_view t = withoutQualifiers;
    if (const std::size_t lt = t.find('<'); lt != std::string_view::npos)
        t = t.substr(0, lt);

    std::string collapsed;
    bool pendingSpace = false;
    for (char c : t)
    {
        if (Lexical::IsWhiteSpace(c))
        {
            pendingSpace = true;
            continue;
        }
        if (pendingSpace && !collapsed.empty())
            collapsed.push_back(' ');
        pendingSpace = false;
        collapsed.push_back(c);
    }
    return std::string(Lexical::LastScopeSegment(collapsed));
}

std::string FollowAliases(std::string name, const std::map<std::string, std::string>& aliases)
{
    for (int depth = 0; depth < kMaxAliasDepth; ++depth)
    {
        const auto it = aliases.find(name);
        if (it == aliases.end())
            break;
        name = Simplify(it->second);
    }
    return name;
}

FieldKind ResolveScalar(std::string_view typeText, const std::map<std::string, std::string>& aliases)
{
    return KnownKind(FollowAliases(Simplify(typeText), aliases));
}

} // namespace

ResolvedKind Resolve(const ReflectedField& field, const std::map<std::string, std::string>& aliases,
                     const EnumTable& enums)
{
    std::string name = Simplify(field.Type);
    if (const EnumDecl* enumDecl = enums.Match(name))
        return {ResolveScalar(enumDecl->Underlying.value_or("int"), aliases), enumDecl, name};

    name = FollowAliases(name, aliases);

    if (field.ArrayExtent && name == "char")
        return {FieldKind::String, nullptr, name};

    if (const EnumDecl* aliasedEnum = enums.Match(name))
        return {ResolveScalar(aliasedEnum->Underlying.value_or("int"), aliases), nullptr, name};

    return {ResolveScalar(name, aliases), nullptr, name};
}

} // namespace GameEngine::ComponentScanner::FieldKindResolver
