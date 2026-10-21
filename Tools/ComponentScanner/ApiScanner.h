#pragma once

#include "ApiModel.h"

#include <filesystem>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The scan's outcome: the description of every marked declaration, and every declaration
// refused. A description with refusals is not written.
struct ApiScanResult
{
    ApiDescription Description;
    std::vector<ApiRefusal> Refusals;
};

// Builds the script API description (Engine/Scripting/engine-api/<Module>.json) from the
// module tree.
//
// `modulesDir` holds one folder per module (Engine/Modules). For each module <M> it reads
// every <M>/Include/<M>/<Name>ScriptApi.h, the headers a script marker may sit in, each into
// the description of the <Name> script module (PhysicsECS/PhysicsScriptApi.h is Physics; two
// headers that name one script module are refused), and searches every other file under
// <M>/Include, and every file under each of `searchDirs` (Engine/Include,
// Engine/ECSModules/*/Include), for a marker with no parse, refusing each hit.
//
// The markers are // line comments, harvested from the raw lines:
//   @ge-script            a free or static function, a struct (a record) or an enum class
//   @ge-script-handle     `using Name = StrongHandle<Tag>;` (or a GenerationalVector::Handle)
//   @ge-script-event <D>  SubscriptionId Subscribe<Name>(scope, ScriptCallback<Payload>), where
//                         <D> is the delivery (PhysicsStep, UiEventRouting); the plain marker on
//                         its void Unsubscribe<Name>(SubscriptionId)
//   @ge-script-try        a bool function with one out parameter: false is an answer
//   @ge-context           the function's first parameter is the system it reaches; the binding
//                         supplies it
//   @ge-engine-thread, @ge-any-thread   the thread rule; the main thread otherwise
// A marker attaches to the declaration on its own line, or to the declaration directly below
// its run of comment lines; a blank line ends the run. The run's /// lines are the entry's
// documentation; a marker on a /// line is refused. A marker in a dead `#if 0` branch or
// inside a /* */ block comment is no marker. Everything the kind table (ApiKindResolver) and these rules do not allow is
// refused with the file, the line and the fix.
namespace ApiScanner
{
ApiScanResult Scan(const std::filesystem::path& modulesDir, const std::vector<std::filesystem::path>& searchDirs);
}

} // namespace GameEngine::ComponentScanner
