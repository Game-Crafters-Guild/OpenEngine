#pragma once

#include "Rendering/Materials/MaterialDocument.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{
// Helpers for mapping MaterialDocument authoring keys (e.g. "baseColor") to
// shader-reflection names (e.g. "uBaseColor") with both:
// - explicit user overrides (doc.bindings)
// - robust auto-matching heuristics (case-insensitive, snake/camel/prefix patterns)
namespace MaterialPropertyBinding
{
// A material property key as the inspector's words: the key read by IdentifierToWords ("baseColor"
// reads "Base Color", "detailUVScale" reads "Detail UV Scale"). Three acronyms the rule reads as words
// are named here: the key "ao" reads "AO", a key ending in "Ior" reads "... IOR" ("specularIor" reads
// "Specular IOR"), and a key starting with "sRGB" reads "sRGB ..." ("sRGBTint" reads "sRGB Tint").
std::string PropertyKeyLabel(std::string_view key);

// Resolve which MaterialDocument property key should be used to source a value
// for the given reflected uniform/member name.
//
// Resolution order:
// 1) explicit mapping from doc.bindings[uniformName] (if present)
// 2) exact key match
// 3) heuristic match against existing doc.properties keys
// 4) suggested key derived from uniform name (even if missing)
std::string ResolvePropertyKeyForUniform(const MaterialDocument& doc, std::string_view uniformName);

// Return true if the two names are equivalent under our heuristics.
bool NamesEquivalent(std::string_view a, std::string_view b);

// Convenience helpers for common value retrieval patterns.
bool TryGetFloat4(const MaterialDocument& doc, std::string_view uniformName, float out4[4]);
} // namespace MaterialPropertyBinding
} // namespace GameEngine::Rendering

