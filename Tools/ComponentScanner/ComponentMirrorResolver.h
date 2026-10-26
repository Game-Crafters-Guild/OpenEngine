#pragma once

#include "ApiModel.h"
#include "ComponentMirror.h"
#include "ReflectedModel.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Builds the C# mirrors of a component scan (ComponentMirror.h has the rules): the class of every
// reflected component, the kind, offset and size of every field of a mirrored one, and the enums
// those fields use, found through the mirror's own enum table. A component the rules refuse is
// named in `outRefusals` with the header, the line and the fix.
namespace ComponentMirrorResolver
{

// `headers` and `reflected` are the component scan's (ReflectedStruct::Source indexes `headers`),
// `aliases` its `using` aliases by simple name. `modulesDir` holds the modules whose Include trees
// the component headers' includes are looked up in for the enum table (IncludeGraph), and
// `description` the script API whose type names a mirror must not take.
ComponentMirrors Resolve(const std::vector<ComponentHeader>& headers, const std::vector<ReflectedStruct>& reflected,
                         const std::map<std::string, std::string>& aliases, const std::filesystem::path& modulesDir,
                         const ApiDescription& description, std::vector<ApiRefusal>& outRefusals);

} // namespace ComponentMirrorResolver

} // namespace GameEngine::ComponentScanner
