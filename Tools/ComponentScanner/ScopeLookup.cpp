#include "ScopeLookup.h"

#include "ApiKindResolver.h"
#include "Lexical.h"

#include <algorithm>

namespace GameEngine::ComponentScanner::ScopeLookup
{
namespace
{

using namespace Lexical;

// "::A::B::C" and "A::B::C" are {"A", "B", "C"}.
std::vector<std::string_view> Segments(std::string_view name)
{
    std::vector<std::string_view> segments;
    if (StartsWith(name, "::"))
        name.remove_prefix(2);
    std::size_t start = 0;
    while (true)
    {
        const std::size_t separator = name.find("::", start);
        segments.push_back(name.substr(start, separator == std::string_view::npos ? std::string_view::npos : separator - start));
        if (separator == std::string_view::npos)
            break;
        start = separator + 2;
    }
    return segments;
}

} // namespace

std::string BareType(std::string_view text)
{
    std::string result;
    for (const std::string& word : SplitWhitespace(text))
    {
        if (word == "const" || word == "volatile" || word == "mutable")
            continue;
        result += (result.empty() ? "" : " ") + word;
    }
    return result;
}


bool NamesFromScope(std::string_view written, std::string_view qualified, std::string_view scope)
{
    if (!ApiKindResolver::NamesQualified(written, qualified))
        return false;
    const std::vector<std::string_view> full = Segments(qualified);
    const std::size_t depth = full.size() - Segments(written).size();
    if (StartsWith(written, "::"))
        return depth == 0;
    const std::vector<std::string_view> scopeSegments = Segments(scope);
    return depth <= scopeSegments.size() &&
           std::equal(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(depth), scopeSegments.begin());
}

std::size_t LookUp(std::string_view written, std::string_view scope, const std::vector<std::string>& names)
{
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (ApiKindResolver::NamesQualified(written, names[i]))
            matches.push_back(i);
    if (matches.empty())
        return kNotFound;
    const std::vector<std::string_view> scopeSegments = Segments(scope);
    const std::size_t writtenCount = Segments(written).size();
    std::size_t best = kNotFound;
    std::size_t bestDepth = 0;
    bool tie = false;
    for (const std::size_t match : matches)
    {
        const std::vector<std::string_view> full = Segments(names[match]);
        const std::size_t depth = full.size() - writtenCount;
        const bool encloses = depth <= scopeSegments.size() &&
                              std::equal(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(depth), scopeSegments.begin());
        if (!encloses)
            continue;
        if (best == kNotFound || depth > bestDepth)
        {
            best = match;
            bestDepth = depth;
            tie = false;
        }
        else if (depth == bestDepth)
            tie = true;
    }
    if (best != kNotFound)
        return tie ? kAmbiguous : best;
    return matches.size() == 1 ? matches.front() : kAmbiguous;
}

} // namespace GameEngine::ComponentScanner::ScopeLookup
