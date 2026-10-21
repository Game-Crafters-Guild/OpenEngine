#pragma once

#include "SourceWarning.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// A #define seen in an active region: its name, the 1-based line of its '#', and its
// replacement text (after the parameter list), comments and literals blanked like the rest
// of the stripped text.
struct MacroDefinition
{
    std::string Name;
    int Line = 0;
    std::string Body;
};

// The source with comments, literals and preprocessor directives removed, plus what the
// removal noticed.
struct StrippedSource
{
    std::string Text;
    std::vector<MacroDefinition> Macros;
    std::vector<SourceWarning> Warnings;
};

// Removes // line comments, /* */ block comments, string, character and raw string literals,
// preprocessor directive lines, and the lines of a literal `#if 0` group (up to its #else,
// #elif or #endif; any other condition is treated as true, so both branches are read). Each
// removed span becomes a single space, so token boundaries survive, and every newline is kept,
// so line numbers in the stripped text match the raw text. This runs before structural parsing
// so braces, semicolons, and commas hidden inside comments, literals or directives cannot
// confuse the brace/field tracker. A string or character literal ends at its line's end (C++
// does not let one span a line); one left open there is reported and the scan continues on the
// next line, as it does after a raw string with no terminator.
namespace CommentStripper
{
StrippedSource Strip(std::string_view source);
}

} // namespace GameEngine::ComponentScanner
