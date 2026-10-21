#pragma once

#include "ApiModel.h"
#include "BindingRules.h"

#include <string>

namespace GameEngine::ComponentScanner
{

// Writes a module's native binding: the header <Module>.script.gen.h (the module's function
// table GE_<Module>_Table_v<format>, its hash, and GE_<Module>_GetInterface) and the translation
// unit <Module>.script.gen.cpp the engine's script exports compile. The unit holds:
//   - a static_assert for each record's size, alignment and field offsets, each enum's
//     underlying type and values, and each scalar field default of a record, against the
//     description;
//   - one thunk per function (BindingRules.h has the crossing): the null and zero-id checks, the
//     out values set to their defaults, DllBootstrap::EnsureEngineInitialized, the thread rule,
//     the context from ScriptingAbi::ScriptContext, the call, the result, and a try/catch to
//     GE_Result_Fail; every refusal sets the last error (Scripting/ScriptThunks.h);
//   - two thunks per event, subscribe and unsubscribe, on the main thread;
//   - the table, filled with the thunks in the description's order, and GE_<Module>_GetInterface,
//     which serves it only to a caller with the same hash;
//   - on the web (__EMSCRIPTEN__), each thunk again as the named export ge_<module>_<function>.
namespace NativeBindingEmitter
{
std::string EmitHeader(const ApiModule& module, const BindingRules::Index& index);
std::string EmitSource(const ApiModule& module, const BindingRules::Index& index);
} // namespace NativeBindingEmitter

} // namespace GameEngine::ComponentScanner
