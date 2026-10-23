#pragma once

#include "WebComponentModel.h"

#include <string>

namespace GameEngine::ComponentScanner
{

// Writes the web library's components.d.ts: per reflected component that is not engine-internal
// (// @ge-engine-internal: engine runtime state, no game API) an exported interface
// (camelCase field names) and a value token `export declare const Name: ComponentType<Name>`
// that entity.get/set/add/remove/has take, intersected with NoAdd for a @ge-no-add component
// (a page reads it but cannot add or remove it); one string union per enum a field uses.
// ComponentType, NoAdd and the view types are imported by name from the library's
// hand-written opengine.d.ts, only the ones the file uses. An entity field is `Entity | null`
// (an unset handle reads as null). 64-bit integers are bigint, so a page never loses bits. A
// float array whose extent is a literal 2, 3, 4, 9 or 16 is a tuple of that length (vectors,
// 3x3 and 4x4 matrices); other arrays are T[].
namespace DtsEmitter
{
std::string Emit(const WebComponentModel& model);
}

} // namespace GameEngine::ComponentScanner
