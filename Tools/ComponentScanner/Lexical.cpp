#include "Lexical.h"

namespace GameEngine::ComponentScanner::Lexical
{

bool IsIdentifier(std::string_view text)
{
    if (text.empty() || !IsIdentifierStart(text[0]))
        return false;
    for (char c : text)
        if (!IsIdentifierChar(c))
            return false;
    return true;
}

std::string_view TrimView(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && IsWhiteSpace(text[begin]))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && IsWhiteSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

std::string Trim(std::string_view text)
{
    return std::string(TrimView(text));
}

std::string TrimStart(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && IsWhiteSpace(text[begin]))
        ++begin;
    return std::string(text.substr(begin));
}

std::string TrimEnd(std::string_view text)
{
    std::size_t end = text.size();
    while (end > 0 && IsWhiteSpace(text[end - 1]))
        --end;
    return std::string(text.substr(0, end));
}

bool StartsWith(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ContainsWord(std::string_view text, std::string_view word)
{
    std::size_t pos = text.find(word);
    while (pos != std::string_view::npos)
    {
        const bool boundaryBefore = pos == 0 || !IsIdentifierChar(text[pos - 1]);
        const std::size_t after = pos + word.size();
        const bool boundaryAfter = after >= text.size() || !IsIdentifierChar(text[after]);
        if (boundaryBefore && boundaryAfter)
            return true;
        pos = text.find(word, pos + 1);
    }
    return false;
}

std::vector<std::string> SplitWhitespace(std::string_view text)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && IsWhiteSpace(text[i]))
            ++i;
        const std::size_t start = i;
        while (i < text.size() && !IsWhiteSpace(text[i]))
            ++i;
        if (i > start)
            tokens.emplace_back(text.substr(start, i - start));
    }
    return tokens;
}

std::vector<std::string> TokenizeDeclarator(std::string_view text)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (IsIdentifierStart(c))
        {
            const std::size_t start = i;
            while (i < text.size() && (IsIdentifierChar(text[i]) || text[i] == ':'))
                ++i;
            tokens.emplace_back(text.substr(start, i - start));
            continue;
        }
        if (c == '*' || c == '&')
            tokens.emplace_back(1, c);
        ++i;
    }
    return tokens;
}

std::string FirstToken(std::string_view text)
{
    const std::string_view trimmed = TrimView(text);
    if (trimmed.empty() || !IsIdentifierStart(trimmed[0]))
        return "";
    std::size_t end = 1;
    while (end < trimmed.size() && IsIdentifierChar(trimmed[end]))
        ++end;
    return std::string(trimmed.substr(0, end));
}

std::string_view LastScopeSegment(std::string_view text)
{
    const std::size_t scope = text.rfind("::");
    return scope == std::string_view::npos ? text : text.substr(scope + 2);
}

std::string ToLowerAscii(std::string_view text)
{
    std::string lowered(text);
    for (char& c : lowered)
        c = ToLower(c);
    return lowered;
}

bool LessIgnoreCase(std::string_view a, std::string_view b)
{
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const unsigned char ca = static_cast<unsigned char>(ToUpper(a[i]));
        const unsigned char cb = static_cast<unsigned char>(ToUpper(b[i]));
        if (ca != cb)
            return ca < cb;
    }
    return a.size() < b.size();
}

} // namespace GameEngine::ComponentScanner::Lexical
