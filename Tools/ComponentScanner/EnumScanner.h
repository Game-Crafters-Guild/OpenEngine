#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// One enumerator: its source identifier and integer value.
struct EnumMember
{
    std::string Name;
    std::int64_t Value = 0;
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
namespace EnumScanner
{
std::vector<EnumDecl> Scan(std::string_view strippedSource);
}

} // namespace GameEngine::ComponentScanner
