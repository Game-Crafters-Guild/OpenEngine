#pragma once

#include "SourceWarning.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// One data member. DeclLine/EndLine are the 1-based source line range the declaration
// spans (from the first token to its terminating ';'), so a marker comment captured from
// the raw text can be associated with the field it annotates. ArrayExtent is the text
// between the first '[' and the last ']' of an array declarator ("3", "kMaxWeights",
// "kCapacity + 1", "4][4" for a two-dimensional array); nullopt for a non-array field.
// Initializer is the default member initializer as written: the text after '=' ("1.0f",
// "{}", "{0.0f, 1.0f, 0.0f}") or a brace initializer with its braces ("{1.0f}"); nullopt when
// the field has none.
struct FieldDecl
{
    std::string Type;
    std::string Name;
    int DeclLine = -1;
    int EndLine = -1;
    std::optional<std::string> ArrayExtent;
    std::optional<std::string> Initializer;
};

struct ParsedStruct
{
    std::string Name;
    std::string Namespace; // fully-qualified, e.g. "GameEngine::Components"

    // 1-based line number of the "struct X" declaration head, so a raw-text struct marker
    // ("// @ge-no-add") can be associated with the struct it precedes. -1 when unknown.
    int DeclLine = -1;

    std::vector<FieldDecl> Fields;
    bool HasVirtual = false;

    // The raw base-clause text (between ':' and '{'), e.g. "public ECS::ComponentBase".
    // Empty when the struct has no bases.
    std::string Bases;

    // Set when the body contains a bare ALL-CAPS macro invocation (e.g. a GE_*_FIELDS
    // field-injection macro). The scanner does not run the preprocessor, so fields hidden
    // behind such a macro are invisible; the struct is skipped rather than emitted with an
    // incomplete field list.
    std::optional<std::string> UnexpandedMacro;
};

// Lightweight C++ struct scanner over comment/literal-stripped source. Tracks namespace
// nesting so each struct gets a fully-qualified enclosing namespace, then brace-matches
// each struct body and extracts its top-level data members. A struct name may hold UTF-8
// letters (the bytes past ASCII), so a caller can name such a struct instead of missing it.
// A namespace nested deeper than kMaxNamespaceDepth is skipped whole with a warning.
namespace StructParser
{
constexpr int kMaxNamespaceDepth = 256;

std::vector<ParsedStruct> Parse(std::string_view source, std::vector<SourceWarning>& outWarnings);
}

} // namespace GameEngine::ComponentScanner
