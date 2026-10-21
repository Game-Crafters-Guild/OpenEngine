#include "CommentStripper.h"

#include "Lexical.h"

#include <utility>

namespace GameEngine::ComponentScanner::CommentStripper
{
namespace
{

constexpr std::size_t kNoMatch = std::string_view::npos;

// The longest delimiter a raw string literal may carry.
constexpr std::size_t kMaxRawDelimiter = 16;

// The stripped text and what the lexer knows about its end: the 1-based line it is on, and the
// identifier-or-number token ([A-Za-z0-9_.]) it ends in: where that token starts and whether it
// starts with a digit, the questions a quote asks, answered as the text is written so each
// answer costs O(1).
struct StrippedText
{
    std::string Text;
    int Line = 1;
    std::size_t TokenStart = 0;
    bool InToken = false;
    bool InNumberToken = false;
};

// One #if, #ifdef or #ifndef group: whether the region around it is read, and whether its
// current branch is. Only a literal 0 condition makes a branch unread.
struct Conditional
{
    bool ParentActive = true;
    bool BranchActive = true;
};

void Append(StrippedText& out, char c)
{
    const bool tokenChar = Lexical::IsIdentifierChar(c) || c == '.';
    if (!tokenChar)
    {
        out.InNumberToken = false;
    }
    else if (!out.InToken)
    {
        out.InNumberToken = Lexical::IsAsciiDigit(c);
        out.TokenStart = out.Text.size();
    }
    out.InToken = tokenChar;
    if (c == '\n')
        ++out.Line;
    out.Text.push_back(c);
}

// The token the stripped text ends in, or "" after any other character.
std::string_view CurrentToken(const StrippedText& out)
{
    return out.InToken ? std::string_view(out.Text).substr(out.TokenStart) : std::string_view();
}

bool IsActive(const std::vector<Conditional>& conditionals)
{
    return conditionals.empty() || (conditionals.back().ParentActive && conditionals.back().BranchActive);
}

// The length of the line continuation (a backslash, then "\n" or "\r\n") at source[i], or 0.
std::size_t ContinuationLength(std::string_view source, std::size_t i)
{
    if (source[i] != '\\')
        return 0;
    if (i + 1 < source.size() && source[i + 1] == '\n')
        return 2;
    if (i + 2 < source.size() && source[i + 1] == '\r' && source[i + 2] == '\n')
        return 3;
    return 0;
}

// The index of the '\n' that ends the line holding source[i], or the source's size.
std::size_t LineEnd(std::string_view source, std::size_t i)
{
    const std::size_t newline = source.find('\n', i);
    return newline == kNoMatch ? source.size() : newline;
}

// Skips the block comment opening at source[i]; every newline inside it is appended to `out`.
// Returns the index past its "*/", or the source's size when it has none.
std::size_t SkipBlockComment(std::string_view source, std::size_t i, StrippedText& out)
{
    const std::size_t n = source.size();
    i += 2;
    while (i + 1 < n && !(source[i] == '*' && source[i + 1] == '/'))
    {
        if (source[i] == '\n')
            Append(out, '\n');
        ++i;
    }
    return i + 2 < n ? i + 2 : n;
}

// Skips the string or character literal whose opening quote is at source[i], escapes and line
// continuations honoured (each continuation's newline is appended to `out`). Returns the index
// past its closing quote with `closed` set, or the index of its line's end with `closed` clear.
std::size_t SkipLiteral(std::string_view source, std::size_t i, StrippedText& out, bool& closed)
{
    const char quote = source[i];
    const std::size_t n = source.size();
    ++i;
    while (i < n && source[i] != '\n')
    {
        if (const std::size_t continuation = ContinuationLength(source, i))
        {
            Append(out, '\n');
            i += continuation;
            continue;
        }
        if (source[i] == quote)
        {
            closed = true;
            return i + 1;
        }
        i += (source[i] == '\\' && i + 1 < n && source[i + 1] != '\n') ? 2 : 1;
    }
    closed = false;
    return i;
}

// R, LR, uR, UR and u8R before a '"' open a raw string literal.
bool IsRawStringPrefix(std::string_view token)
{
    return token == "R" || token == "LR" || token == "uR" || token == "UR" || token == "u8R";
}

// Skips the raw string literal R"delimiter( ... )delimiter" whose opening quote is at
// source[i]; every newline inside it is appended to `out`. Returns the index past its closing
// quote, or npos when the delimiter is not valid or the terminator never comes.
std::size_t SkipRawString(std::string_view source, std::size_t i, StrippedText& out)
{
    const std::size_t open = source.substr(i + 1, kMaxRawDelimiter + 1).find('(');
    if (open == kNoMatch)
        return kNoMatch;
    const std::string_view delimiter = source.substr(i + 1, open);
    for (char c : delimiter)
        if (c == ')' || c == '\\' || c == '"' || Lexical::IsWhiteSpace(c))
            return kNoMatch;

    const std::string terminator = ")" + std::string(delimiter) + "\"";
    const std::size_t bodyStart = i + 1 + open + 1;
    const std::size_t close = source.find(terminator, bodyStart);
    if (close == kNoMatch)
        return kNoMatch;
    for (std::size_t k = bodyStart; k < close; ++k)
        if (source[k] == '\n')
            Append(out, '\n');
    return close + terminator.size();
}

// Reads the directive whose '#' is at source[i] up to the end of its logical line (line
// continuations and block comments may carry it over several). Its text after the '#' goes to
// `outText` with comments, literals and continuations as single spaces; every newline it spans
// is appended to `out`. A literal left open in a directive (the apostrophe of "#error don't")
// ends at the line's end without a warning: the directive's text is not code the scanner reads.
// Returns the index of the '\n' that ends it, or the source's size.
std::size_t ReadDirective(std::string_view source, std::size_t i, StrippedText& out, std::string& outText)
{
    const std::size_t n = source.size();
    ++i;
    while (i < n && source[i] != '\n')
    {
        if (const std::size_t continuation = ContinuationLength(source, i))
        {
            Append(out, '\n');
            outText.push_back(' ');
            i += continuation;
            continue;
        }
        const char c = source[i];
        if (c == '/' && i + 1 < n && source[i + 1] == '/')
            return LineEnd(source, i);
        if (c == '/' && i + 1 < n && source[i + 1] == '*')
        {
            i = SkipBlockComment(source, i, out);
            outText.push_back(' ');
            continue;
        }
        if (c == '"' || c == '\'')
        {
            bool closed = false;
            i = SkipLiteral(source, i, out, closed);
            outText.push_back(' ');
            continue;
        }
        outText.push_back(c);
        ++i;
    }
    return i;
}

// Applies one directive's text (after the '#'): conditional groups open, switch branch and
// close; a #define in a read region is recorded. `line` is the line of its '#'.
void ApplyDirective(std::string_view text, int line, std::vector<Conditional>& conditionals, StrippedSource& result)
{
    const std::string_view body = Lexical::TrimView(text);
    std::size_t nameEnd = 0;
    while (nameEnd < body.size() && Lexical::IsIdentifierChar(body[nameEnd]))
        ++nameEnd;
    const std::string_view name = body.substr(0, nameEnd);
    const std::string_view rest = Lexical::TrimView(body.substr(nameEnd));

    if (name == "if" || name == "ifdef" || name == "ifndef")
    {
        const bool literalZero = name == "if" && rest == "0";
        conditionals.push_back(Conditional{IsActive(conditionals), !literalZero});
        return;
    }
    if (name == "elif" || name == "elifdef" || name == "elifndef")
    {
        if (!conditionals.empty())
            conditionals.back().BranchActive = !(name == "elif" && rest == "0");
        return;
    }
    if (name == "else")
    {
        if (!conditionals.empty())
            conditionals.back().BranchActive = true;
        return;
    }
    if (name == "endif")
    {
        if (!conditionals.empty())
            conditionals.pop_back();
        return;
    }
    if (name != "define" || !IsActive(conditionals))
        return;

    std::size_t macroNameEnd = 0;
    while (macroNameEnd < rest.size() && Lexical::IsIdentifierChar(rest[macroNameEnd]))
        ++macroNameEnd;
    if (macroNameEnd == 0)
        return;
    std::string_view replacement = rest.substr(macroNameEnd);
    // A '(' right after the name opens a function-like macro's parameter list.
    if (!replacement.empty() && replacement[0] == '(')
    {
        const std::size_t close = replacement.find(')');
        replacement = close == kNoMatch ? std::string_view() : replacement.substr(close + 1);
    }
    result.Macros.push_back(
        MacroDefinition{std::string(rest.substr(0, macroNameEnd)), line, Lexical::Trim(replacement)});
}

} // namespace

StrippedSource Strip(std::string_view source)
{
    StrippedSource result;
    StrippedText out;
    out.Text.reserve(source.size());
    std::vector<Conditional> conditionals;
    // Only whitespace and comments since the last newline: a '#' here opens a directive.
    bool atLineStart = true;
    const std::size_t n = source.size();
    std::size_t i = 0;

    while (i < n)
    {
        const char c = source[i];

        if (c == '\n')
        {
            Append(out, c);
            atLineStart = true;
            ++i;
            continue;
        }

        if (c == '#' && atLineStart)
        {
            const int line = out.Line;
            std::string text;
            i = ReadDirective(source, i, out, text);
            ApplyDirective(text, line, conditionals, result);
            Append(out, ' ');
            continue;
        }

        // Inside an unread branch a string or character literal is still skipped (to its line's
        // end when left open there, without a warning: the preprocessor does not diagnose skipped
        // text), so a "/*" inside one opens no comment that would run past the #endif.
        if (!IsActive(conditionals) && (c == '"' || c == '\''))
        {
            bool closed = false;
            i = SkipLiteral(source, i, out, closed);
            atLineStart = false;
            continue;
        }

        // Line comment: dropped up to (not including) the newline.
        if (c == '/' && i + 1 < n && source[i + 1] == '/')
        {
            i = LineEnd(source, i);
            continue;
        }

        // Block comment: newlines inside it are kept.
        if (c == '/' && i + 1 < n && source[i + 1] == '*')
        {
            i = SkipBlockComment(source, i, out);
            Append(out, ' ');
            continue;
        }

        atLineStart = atLineStart && Lexical::IsWhiteSpace(c);

        // Inside an unread branch only newlines, comments, literals and directives count; the rest
        // is dropped, as the preprocessor drops it.
        if (!IsActive(conditionals))
        {
            ++i;
            continue;
        }

        // A digit separator (0xFFFF'FFFF, 1'000) continues a number literal and is dropped, so
        // the number reads as one token (1'000 -> 1000); after any other token (u8'a', L'x') a
        // quote opens a character literal.
        if (c == '\'' && out.InNumberToken)
        {
            ++i;
            continue;
        }

        if (c == '"' && IsRawStringPrefix(CurrentToken(out)))
        {
            const int line = out.Line;
            const std::size_t end = SkipRawString(source, i, out);
            if (end == kNoMatch)
            {
                result.Warnings.push_back(SourceWarning{
                    line, "a raw string literal has no closing )delimiter\" or its delimiter is not valid; the scan "
                          "continues at the next line. Close it with )delimiter\" and a delimiter of at most 16 "
                          "characters without spaces, parentheses or backslashes"});
                i = LineEnd(source, i);
            }
            else
            {
                i = end;
            }
            Append(out, ' ');
            continue;
        }

        if (c == '"' || c == '\'')
        {
            const int line = out.Line;
            bool closed = false;
            i = SkipLiteral(source, i, out, closed);
            if (!closed)
                result.Warnings.push_back(SourceWarning{
                    line, "a string or character literal is not closed on its line (C++ ends one there); the scan "
                          "continues at the next line. Close the literal on its line"});
            Append(out, ' ');
            continue;
        }

        Append(out, c);
        ++i;
    }

    result.Text = std::move(out.Text);
    return result;
}

} // namespace GameEngine::ComponentScanner::CommentStripper
