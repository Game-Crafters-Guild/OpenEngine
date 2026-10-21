#include "EnumScanner.h"

#include "Lexical.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace GameEngine::ComponentScanner::EnumScanner
{
namespace
{

using namespace Lexical;

constexpr std::size_t kNoMatch = std::string_view::npos;

std::size_t SkipWhitespace(std::string_view src, std::size_t pos)
{
    while (pos < src.size() && IsWhiteSpace(src[pos]))
        ++pos;
    return pos;
}

// Parse a C++ integer literal: optional sign, decimal or 0x hex, optional u/l suffixes.
// Returns nullopt for anything else (an expression the scanner cannot evaluate). Char
// literals never reach here — CommentStripper removes string/char literals first.
std::optional<std::int64_t> TryParseIntLiteral(std::string_view raw)
{
    std::string_view s = TrimView(raw);
    if (s.empty())
        return std::nullopt;

    bool negative = false;
    if (s[0] == '+' || s[0] == '-')
    {
        negative = s[0] == '-';
        s = TrimView(s.substr(1));
    }
    if (s.empty())
        return std::nullopt;

    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == 'u' || s[end - 1] == 'U' || s[end - 1] == 'l' || s[end - 1] == 'L'))
        --end;
    std::string_view digits = TrimView(s.substr(0, end));
    if (digits.empty())
        return std::nullopt;

    std::int64_t parsed = 0;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        const std::string_view hex = digits.substr(2);
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
        if (ec != std::errc{} || ptr != hex.data() + hex.size() || hex.empty())
            return std::nullopt;
        parsed = static_cast<std::int64_t>(value);
    }
    else
    {
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, 10);
        if (ec != std::errc{} || ptr != digits.data() + digits.size())
            return std::nullopt;
    }
    // The negation of the most negative value has no int64 (0x8000000000000000 negated).
    if (negative && parsed == std::numeric_limits<std::int64_t>::min())
        return std::nullopt;
    return negative ? -parsed : parsed;
}

// Split on commas at bracket/paren depth 0 — defensive against a parenthesized value like `(1)`.
std::vector<std::string_view> SplitTopLevel(std::string_view body)
{
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        if (c == '(' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == ']' || c == '>')
        {
            if (depth > 0)
                --depth;
        }
        else if (c == ',' && depth == 0)
        {
            parts.push_back(body.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(body.substr(start));
    return parts;
}

// Parse the comma-separated body. Returns nullopt if any enumerator value can't be resolved
// to an integer literal (the enum is then skipped for name-serialization).
std::optional<std::vector<EnumMember>> ParseMembers(std::string_view body)
{
    std::vector<EnumMember> members;
    // The implicit value of the next enumerator; nullopt after the int64 maximum, where C++
    // has no next value either.
    std::optional<std::int64_t> next = 0;
    for (std::string_view raw : SplitTopLevel(body))
    {
        const std::string_view item = TrimView(raw);
        if (item.empty())
            continue; // trailing comma / blank

        const std::size_t eq = item.find('=');
        const std::string_view namePart = TrimView(eq == kNoMatch ? item : item.substr(0, eq));
        if (!IsIdentifier(namePart))
            return std::nullopt;

        if (eq == kNoMatch && !next)
            return std::nullopt;
        std::int64_t value = next.value_or(0);
        if (eq != kNoMatch)
        {
            const std::optional<std::int64_t> parsed = TryParseIntLiteral(item.substr(eq + 1));
            if (!parsed)
                return std::nullopt;
            value = *parsed;
        }

        members.push_back(EnumMember{std::string(namePart), value});
        if (value == std::numeric_limits<std::int64_t>::max())
            next = std::nullopt;
        else
            next = value + 1;
    }
    return members;
}

// Name, optional underlying type and body of the enum head starting after "enum" and its
// whitespace at `pos`, with `pos` at the first character of either the class/struct key or
// the name. Returns the index just past the closing '}' or npos.
std::size_t MatchEnumRest(std::string_view src, std::size_t pos, std::string& outName,
                          std::optional<std::string>& outUnderlying, std::string& outBody)
{
    std::size_t i = pos;
    if (i >= src.size() || !IsIdentifierStart(src[i]))
        return kNoMatch;
    const std::size_t nameStart = i;
    while (i < src.size() && IsIdentifierChar(src[i]))
        ++i;
    const std::string_view name = src.substr(nameStart, i - nameStart);
    i = SkipWhitespace(src, i);
    if (i >= src.size())
        return kNoMatch;

    std::optional<std::string> underlying;
    if (src[i] == ':')
    {
        const std::size_t start = i + 1;
        std::size_t j = start;
        while (j < src.size() && src[j] != '{' && src[j] != '}' && src[j] != ';')
            ++j;
        if (j == start || j >= src.size() || src[j] != '{')
            return kNoMatch;
        underlying = std::string(src.substr(start, j - start));
        i = j;
    }
    if (src[i] != '{')
        return kNoMatch;

    const std::size_t bodyStart = i + 1;
    std::size_t j = bodyStart;
    while (j < src.size() && src[j] != '{' && src[j] != '}')
        ++j;
    if (j >= src.size() || src[j] != '}')
        return kNoMatch;

    outName.assign(name);
    outUnderlying = std::move(underlying);
    outBody.assign(src.substr(bodyStart, j - bodyStart));
    return j + 1;
}

// "enum [class|struct] Name [: underlying] { body }" at `pos`. A "class"/"struct" key that
// is not followed by a valid head is re-read as the enum's name.
std::size_t MatchEnum(std::string_view src, std::size_t pos, std::string& outName,
                      std::optional<std::string>& outUnderlying, std::string& outBody, bool& outScoped)
{
    outScoped = false;
    constexpr std::string_view kEnum = "enum";
    if (pos > 0 && IsIdentifierChar(src[pos - 1]))
        return kNoMatch;
    if (src.substr(pos, kEnum.size()) != kEnum)
        return kNoMatch;
    std::size_t i = pos + kEnum.size();
    if (i >= src.size() || !IsWhiteSpace(src[i]))
        return kNoMatch;
    i = SkipWhitespace(src, i);

    for (std::string_view key : {std::string_view("class"), std::string_view("struct")})
    {
        if (src.substr(i, key.size()) == key && i + key.size() < src.size() && IsWhiteSpace(src[i + key.size()]))
        {
            const std::size_t afterKey = SkipWhitespace(src, i + key.size());
            if (const std::size_t end = MatchEnumRest(src, afterKey, outName, outUnderlying, outBody); end != kNoMatch)
            {
                outScoped = true;
                return end;
            }
        }
    }
    return MatchEnumRest(src, i, outName, outUnderlying, outBody);
}

} // namespace

std::vector<EnumDecl> Scan(std::string_view strippedSource)
{
    std::vector<EnumDecl> result;
    // The line of `countedTo`, advanced as the scan moves forward.
    int line = 1;
    std::size_t countedTo = 0;
    std::size_t pos = strippedSource.find("enum");
    while (pos != kNoMatch)
    {
        std::string name;
        std::optional<std::string> underlying;
        std::string body;
        bool scoped = false;
        const std::size_t end = MatchEnum(strippedSource, pos, name, underlying, body, scoped);
        if (end == kNoMatch)
        {
            pos = strippedSource.find("enum", pos + 1);
            continue;
        }

        if (std::optional<std::vector<EnumMember>> members = ParseMembers(body); members && !members->empty())
        {
            EnumDecl decl;
            decl.Name = std::move(name);
            decl.Members = std::move(*members);
            decl.Scoped = scoped;
            line += static_cast<int>(std::count(strippedSource.begin() + static_cast<std::ptrdiff_t>(countedTo),
                                                strippedSource.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
            countedTo = pos;
            decl.DeclLine = line;
            if (underlying)
                decl.Underlying = Trim(*underlying);
            result.push_back(std::move(decl));
        }
        pos = strippedSource.find("enum", end);
    }
    return result;
}

} // namespace GameEngine::ComponentScanner::EnumScanner
