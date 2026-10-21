#pragma once

#include "EnumScanner.h"
#include "MarkerScanner.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// One reflected field: its member name, its C++ type string (used to match enum fields to a
// scanned enum declaration and to resolve the web kind), its array extent text (nullopt when
// not an array), and its markers: the FieldFlags policy (Hidden / ReadOnly / Transient), the
// tooltip and the inspector range (nullopt when unmarked).
struct ReflectedField
{
    std::string Name;
    std::string Type;
    std::optional<std::string> ArrayExtent;
    bool Hidden = false;
    bool ReadOnly = false;
    bool Transient = false;
    std::optional<std::string> Tooltip;
    std::optional<FieldRange> Range;
};

struct ReflectedStruct
{
    std::string Name;
    std::string Namespace;
    std::vector<ReflectedField> Fields;
    std::string Header;
    bool NoAdd = false;
    bool DoNotSerialize = false;
    bool EditorOnly = false;
};

// One detected user system: an ECS::SystemBase-derived struct. Unlike a component it carries
// no reflected fields (a system is not a POD); only the type name and header are needed to
// emit its RegisterUserSystem line.
struct ReflectedSystem
{
    std::string Name;
    std::string Namespace;
    std::string Header;
};

// Enum declarations discovered across all scanned headers, keyed by simple (unqualified)
// name, plus the names seen with conflicting members: those are dropped so their fields fall
// back to integer serialization rather than risk a wrong name table.
struct EnumTable
{
    std::map<std::string, EnumDecl> ByName;
    std::set<std::string> Ambiguous;

    // Resolve a field's C++ type string to a scanned enum declaration, or nullptr if it is not
    // a known (non-ambiguous) enum. Strips qualifiers and namespace: the last
    // whitespace-separated token, then its last "::" segment — "const Foo::Bar" matches "Bar".
    const EnumDecl* Match(std::string_view fieldType) const;
};

inline std::string QualifiedName(std::string_view ns, std::string_view name)
{
    return ns.empty() ? std::string(name) : std::string(ns) + "::" + std::string(name);
}

} // namespace GameEngine::ComponentScanner
