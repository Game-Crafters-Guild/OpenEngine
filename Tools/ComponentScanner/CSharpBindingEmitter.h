#pragma once

#include "ApiModel.h"
#include "BindingRules.h"

#include <string>

namespace GameEngine::ComponentScanner
{

// Writes a module's C# binding, <Module>.g.cs, into the GameEngine.Scripting.Bindings assembly:
// its enums, its records (explicit layout at the description's offsets and size, with the field
// defaults), and one static partial class per module whose methods call the module's native
// table through `delegate* unmanaged[Cdecl]` pointers resolved once per process
// (BindingTable.Acquire, which refuses a table with another hash). No runtime marshalling, no
// reflection: the assembly carries [assembly: DisableRuntimeMarshalling], so a record crosses as
// its bytes. Every name and type comes from BindingRules.h.
namespace CSharpBindingEmitter
{
std::string Emit(const ApiModule& module, const BindingRules::Index& index);

// The doc text as <summary> lines at `indent`, one line of the C++ comment per line.
void EmitSummary(std::string& out, std::string_view doc, std::string_view indent);
} // namespace CSharpBindingEmitter

} // namespace GameEngine::ComponentScanner
