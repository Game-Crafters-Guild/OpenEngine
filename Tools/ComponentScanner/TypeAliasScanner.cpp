#include "TypeAliasScanner.h"

#include "Lexical.h"

namespace GameEngine::ComponentScanner::TypeAliasScanner
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

// "using Name = Target;" at `pos`; Target runs to the ';' and holds no ';', '{' or '}'.
// Returns the index just past the ';' or npos.
std::size_t MatchAlias(std::string_view src, std::size_t pos, std::string& outName, std::string& outTarget)
{
    constexpr std::string_view kUsing = "using";
    if (pos > 0 && IsIdentifierChar(src[pos - 1]))
        return kNoMatch;
    if (src.substr(pos, kUsing.size()) != kUsing)
        return kNoMatch;
    std::size_t i = pos + kUsing.size();
    if (i >= src.size() || !IsWhiteSpace(src[i]))
        return kNoMatch;
    i = SkipWhitespace(src, i);
    if (i >= src.size() || !IsIdentifierStart(src[i]))
        return kNoMatch;
    const std::size_t nameStart = i;
    while (i < src.size() && IsIdentifierChar(src[i]))
        ++i;
    const std::string_view name = src.substr(nameStart, i - nameStart);
    i = SkipWhitespace(src, i);
    if (i >= src.size() || src[i] != '=')
        return kNoMatch;
    const std::size_t afterEquals = i + 1;
    i = SkipWhitespace(src, afterEquals);
    const std::size_t targetStart = i;
    while (i < src.size() && src[i] != ';' && src[i] != '{' && src[i] != '}')
        ++i;
    // At least one character between '=' and ';', whitespace included.
    if (i == afterEquals || i >= src.size() || src[i] != ';')
        return kNoMatch;
    outName.assign(name);
    outTarget = Trim(src.substr(targetStart, i - targetStart));
    return i + 1;
}

} // namespace

std::vector<std::pair<std::string, std::string>> Scan(std::string_view strippedSource)
{
    std::vector<std::pair<std::string, std::string>> aliases;
    std::size_t pos = strippedSource.find("using");
    while (pos != kNoMatch)
    {
        std::string name;
        std::string target;
        const std::size_t end = MatchAlias(strippedSource, pos, name, target);
        if (end == kNoMatch)
        {
            pos = strippedSource.find("using", pos + 1);
            continue;
        }
        aliases.emplace_back(std::move(name), std::move(target));
        pos = strippedSource.find("using", end);
    }
    return aliases;
}

} // namespace GameEngine::ComponentScanner::TypeAliasScanner
