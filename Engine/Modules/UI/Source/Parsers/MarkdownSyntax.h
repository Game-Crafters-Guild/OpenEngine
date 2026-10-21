#pragma once

// Internal to the Markdown parser's translation units: MarkdownParser.cpp reads the blocks,
// MarkdownInlineParser.cpp the inline content of each. Character classes are ASCII; a byte of a
// multi-byte UTF-8 sequence is neither whitespace nor punctuation, so it reads as a letter.

#include "MarkdownDocument.h"

#include <cstddef>
#include <string_view>

namespace GameEngine { namespace UIParsing {

inline bool IsSpaceOrTab(char c)
{
    return c == ' ' || c == '\t';
}

inline bool IsWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool IsAsciiPunctuation(char c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

inline std::string_view TrimEnd(std::string_view text)
{
    while (!text.empty() && IsWhitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

inline size_t CountRun(std::string_view text, size_t at, char c)
{
    size_t end = at;
    while (end < text.size() && text[end] == c)
        ++end;
    return end - at;
}

/// A web address (http or https): the only addresses the parser makes links of. Any other
/// address shows as its text.
inline bool IsWebAddress(std::string_view text)
{
    return text.starts_with("http://") || text.starts_with("https://");
}

/// Parses one block's inline content: strong, emphasis, code spans, links, autolinks, bare
/// web addresses, escapes and line breaks.
[[nodiscard]] MarkdownInlines ParseMarkdownInlines(std::string_view text);

}} // namespace GameEngine::UIParsing
