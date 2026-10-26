#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// Name lookup over qualified C++ names, as the compiler looks a written name up from a scope.
// The component mirror finds structs, enums and constants with it; the reflection scan finds
// the enums its fields name in the headers a component header includes.
namespace GameEngine::ComponentScanner::ScopeLookup
{

// LookUp's results besides an index.
constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kAmbiguous = kNotFound - 1;

// The type text without cv words, its spaces collapsed: "const Mathematics::Vector3" is
// "Mathematics::Vector3".
std::string BareType(std::string_view text);

// True when `written`, looked up from `scope` as C++ looks a name up, names the type `qualified`:
// its segments end the qualified name and the namespaces it leaves out enclose the scope. A name
// written from the root (`::GameEngine::GUID`) is the whole qualified name.
bool NamesFromScope(std::string_view written, std::string_view qualified, std::string_view scope);

// The index in `names` of the qualified name `written` names when C++ looks it up from `scope`: of
// the names whose segments end in the written ones, the one in the innermost namespace or class
// around the scope wins; a single match anywhere else is taken. kNotFound, or kAmbiguous when two
// tie.
std::size_t LookUp(std::string_view written, std::string_view scope, const std::vector<std::string>& names);

} // namespace GameEngine::ComponentScanner::ScopeLookup
