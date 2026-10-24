#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// One enumerator: its source identifier, its integer value, the 1-based line of its identifier
// and its "// @ge-label Text" (nullopt when unmarked; the scanner attaches it from the raw lines).
struct EnumMember
{
    std::string Name;
    std::int64_t Value = 0;
    int Line = -1;
    std::optional<std::string> Label;
};

// A parsed enum declaration: its simple (unqualified) name, ordered enumerators, the
// declared underlying type text ("uint8", "std::uint32_t") or nullopt when none is written,
// whether it is scoped (`enum class` / `enum struct`), and the 1-based line of its `enum`
// keyword.
struct EnumDecl
{
    std::string Name;
    std::vector<EnumMember> Members;
    std::optional<std::string> Underlying;
    bool Scoped = false;
    int DeclLine = -1;
};

// Discovers C++ `enum` / `enum class` declarations and their enumerator name<->value pairs
// so the scanner can emit a constexpr name table for any enum used by a reflected component
// field. Runs over COMMENT-STRIPPED source (CommentStripper::Strip).
//
// Handles: `enum class`, `enum struct`, plain `enum`; an optional `: underlying` type;
// implicit values (auto-increment from 0) and explicit decimal / hex (0x) literals;
// trailing commas. An enum whose any value is a non-literal expression (`1 << 2`, `A | B`,
// `sizeof(x)`) is dropped — the caller then leaves that enum's fields as plain integers,
// which is the right outcome for flag enums, which have no single serializable name.
// An enum declared at namespace or class scope, with its qualified name ("GameEngine::Pathfinding::GridType")
// and the scope it is declared in ("GameEngine::Pathfinding").
struct ScopedEnumDecl
{
    EnumDecl Decl;
    std::string CppName;
    std::string Scope;
};

namespace EnumScanner
{
std::vector<EnumDecl> Scan(std::string_view strippedSource);

// Scan's enums with their scopes; an enum inside a function body or another brace block is left out.
std::vector<ScopedEnumDecl> ScanScoped(std::string_view strippedSource);
}

} // namespace GameEngine::ComponentScanner
