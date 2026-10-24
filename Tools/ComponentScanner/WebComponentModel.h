#pragma once

#include "FieldKind.h"
#include "ReflectedModel.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// One field as a web page sees it: its reflected name and its TypeScript name (camelCase),
// its kind, the enum whose names it takes (nullptr for a non-enum field), its array extent
// text (nullopt when not an array; a String field's extent is its byte capacity), and the
// markers that reach the page.
struct WebField
{
    std::string Name;
    std::string TsName;
    FieldKind Kind = FieldKind::Unknown;
    const EnumDecl* Enum = nullptr;
    std::optional<std::string> ArrayExtent;
    bool ReadOnly = false;
    std::optional<std::string> Tooltip;
    std::optional<FieldRange> Range;
};

// NoAdd: the component is marked // @ge-no-add (every entity has one; a page cannot add or
// remove it). EngineInternal: the component is marked // @ge-engine-internal (engine runtime
// state, no game API): components.json lists it, marked, and components.d.ts leaves it out.
struct WebComponent
{
    std::string Name;
    bool NoAdd = false;
    bool EngineInternal = false;
    std::vector<WebField> Fields;
};

// Why a field is left out of the web types. NoReflectedKind: its type is a struct declared in
// the scanned headers with no FieldTypeTraits, so the registry reports Unknown too.
// UnresolvedType: its type is not declared in the scanned headers, so the scanner cannot tell
// what the registry reports: an enum declared outside the scanned roots or a platform-sized
// integer (long, size_t) has an integer kind there, a struct is Unknown. Omitted therefore
// means "absent from the web types", never "Unknown in the registry".
enum class WebOmissionReason
{
    NoReflectedKind,
    UnresolvedType,
};

// A field left out of the web types, listed so the gap is counted rather than hidden behind
// an untyped field.
struct WebOmission
{
    std::string Component;
    std::string Field;
    std::string Type;
    WebOmissionReason Reason = WebOmissionReason::NoReflectedKind;
};

// The component model the web emitters write (--emit-dts, --emit-json), built from the same
// reflected structs the C++ registration is emitted from. A game export strips editor-only
// components from every scene, so they are left out; so are hidden fields (no page-facing
// surface) and [DoNotSerialize] fields (runtime state the engine rebuilds). A field the
// scanner cannot type is never emitted untyped: it is left out and listed in Omitted.
// Components are sorted by name, fields keep declaration order, Enums lists every enum a
// kept field uses, by name.
struct WebComponentModel
{
    std::vector<WebComponent> Components;
    std::vector<const EnumDecl*> Enums;
    std::vector<WebOmission> Omitted;

    // Builds the model, or returns nullopt with one message per declaration the web types
    // cannot express at all: a multi-dimensional array field, or an enum named like a
    // component (both would be one exported name in components.d.ts). knownStructs holds the
    // simple names of every struct declared in the scanned headers, in any namespace. The
    // model points into `enums`, which must outlive it.
    static std::optional<WebComponentModel> Build(const std::vector<ReflectedStruct>& reflected,
                                                  const std::map<std::string, std::string>& aliases,
                                                  const EnumTable& enums, const std::set<std::string>& knownStructs,
                                                  std::vector<std::string>& errors);
};

} // namespace GameEngine::ComponentScanner
