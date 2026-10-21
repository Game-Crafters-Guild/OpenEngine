#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Collects `using Name = Target;` alias declarations from COMMENT-STRIPPED source so a field
// typed through an alias (`MaterialRef`, an alias of `AssetRef<AssetType::Material>`)
// resolves to the kind of its target. Each entry is the alias's simple name and the target
// as written (trimmed).
namespace TypeAliasScanner
{
std::vector<std::pair<std::string, std::string>> Scan(std::string_view strippedSource);
}

} // namespace GameEngine::ComponentScanner
