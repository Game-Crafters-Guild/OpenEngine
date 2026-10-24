#pragma once

#include "WebComponentModel.h"

#include <string>

namespace GameEngine::ComponentScanner
{

// Writes the component model as JSON, the machine-readable twin of components.d.ts, which also
// lists the engine-internal components, marked "engineInternal": true:
//   { "components": [ { "name", "engineInternal"?, "fields": [ { "name", "tsName", "kind", "enum"?, "count"?,
//                                              "readonly"?, "tooltip"?, "label"?, "min"?, "max"? } ] } ],
//     "enums": [ { "name", "values": [ { "name", "value", "label"? } ] } ],
//     "omitted": [ { "component", "field", "type", "reason" } ] }
// "name" is the reflected field name, "tsName" the components.d.ts one. "kind" is the
// FieldTypeId name the registration gets (an enum field reports its underlying integer kind
// and names its enum); "count" is an array's extent, a number when the source writes a
// literal and the extent expression otherwise. Optional keys appear only when set. "omitted"
// lists the fields left out of the web types with their C++ type and why:
// "noReflectedKind" (the registry reports Unknown too) or "unresolvedType" (the scanner
// cannot tell what the registry reports; see WebOmissionReason).
namespace JsonEmitter
{
std::string Emit(const WebComponentModel& model);
}

} // namespace GameEngine::ComponentScanner
