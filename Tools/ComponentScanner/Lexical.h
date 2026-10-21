#pragma once

#include <string>
#include <string_view>
#include <vector>

// Byte-level lexical helpers shared by the scanner's passes. The scanner works on the
// header bytes as read (UTF-8); every classification below is ASCII, which is what the
// identifiers, keywords and marker payloads it inspects are made of.
namespace GameEngine::ComponentScanner::Lexical
{

inline bool IsWhiteSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

inline bool IsAsciiLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool IsAsciiDigit(char c)
{
    return c >= '0' && c <= '9';
}

inline bool IsUpper(char c)
{
    return c >= 'A' && c <= 'Z';
}

inline char ToLower(char c)
{
    return IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

inline char ToUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// [A-Za-z_]
inline bool IsIdentifierStart(char c)
{
    return IsAsciiLetter(c) || c == '_';
}

// [A-Za-z0-9_]
inline bool IsIdentifierChar(char c)
{
    return IsIdentifierStart(c) || IsAsciiDigit(c);
}

// ^[A-Za-z_][A-Za-z0-9_]*$
bool IsIdentifier(std::string_view text);

std::string_view TrimView(std::string_view text);
std::string Trim(std::string_view text);
std::string TrimStart(std::string_view text);
std::string TrimEnd(std::string_view text);

bool StartsWith(std::string_view text, std::string_view prefix);
bool EndsWith(std::string_view text, std::string_view suffix);

// True when `word` occurs in `text` delimited by non-identifier characters on both sides
// (the \bword\b test).
bool ContainsWord(std::string_view text, std::string_view word);

// Whitespace-separated tokens, empty entries dropped.
std::vector<std::string> SplitWhitespace(std::string_view text);

// Identifiers with optional :: qualification, plus standalone '*' and '&' tokens; every
// other character is skipped ([A-Za-z_][A-Za-z0-9_:]*|\*|&).
std::vector<std::string> TokenizeDeclarator(std::string_view text);

// ^[A-Za-z_][A-Za-z0-9_]* of the text after leading whitespace, or "".
std::string FirstToken(std::string_view text);

// "A::B::C" -> "C"; a text without "::" is returned as is.
std::string_view LastScopeSegment(std::string_view text);

std::string ToLowerAscii(std::string_view text);

// Ordinal comparison that folds ASCII letters to upper case first ('_' sorts after 'Z',
// before 'a' folds away): the order the header list is scanned in.
bool LessIgnoreCase(std::string_view a, std::string_view b);

} // namespace GameEngine::ComponentScanner::Lexical
