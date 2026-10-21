#pragma once

#include "ApiModel.h"

#include <cstdint>
#include <string>

namespace GameEngine::ComponentScanner
{

// Writes one module's script API description, Engine/Scripting/engine-api/<Module>.json:
//   { "format": 1, "name", "hash",
//     "constants": [ { "id", "name", "cpp", "header", "line", "kind", "value" } ],
//     "enums":     [ { "id", "name", "cpp", "header", "line", "doc", "underlying", "flags"?,
//                      "values": [ { "name", "value" } ] } ],
//     "events":    [ { "id", "name", "header", "line", "doc", "scope", "payload", "delivery",
//                      "subscribe", "unsubscribe", "context"? } ],
//     "functions": [ { "id", "name", "cpp", "header", "line", "doc", "thread", "try"?,
//                      "context"?, "returns", "params": [ { "name", "kind", ..., "out"?,
//                      "default"?, "defaultConstant"? } ] } ],
//     "handles":   [ { "id", "name", "cpp", "header", "line", "doc", "width" } ],
//     "records":   [ { "id", "name", "cpp", "header", "line", "doc", "size", "align",
//                      "fields": [ { "name", "kind", ..., "offset", "default"?,
//                                    "defaultConstant"? } ] } ] }
// A type is written in place as "kind" plus "type" (the id of a marked record, enum or handle)
// or "element" (a span's element type). Every list is sorted by name and every id is
// "<Module>.<Name>", so the text changes only when a marked declaration does. A default is
// written in one spelling per kind: true or false, a decimal integer, the shortest float or
// double, an enumerator's name, or {} for a record; a Mathematics or color field lists its
// components in memory order. "hash" is the FNV-1a 64 of the module's canonical form
// (ModuleHashInput): its names, kinds, order, layouts, defaults, threads and deliveries,
// without documentation, source lines or the context type.
namespace ApiJsonEmitter
{
std::string EmitModule(const ApiModule& module);

// The canonical text a module's hash is computed over.
std::string ModuleHashInput(const ApiModule& module);

std::uint64_t ModuleHash(const ApiModule& module);
} // namespace ApiJsonEmitter

} // namespace GameEngine::ComponentScanner
