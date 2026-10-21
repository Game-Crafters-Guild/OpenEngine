#include "MarkupSearchProvider.h"

#include "MarkupContext.h"

#include "Types/StringUtils.h"

#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr const char* kSeparator = " \xC2\xB7 ";

// 0xFFRRGGBB from markup_list's "#RRGGBB" color; 0 when the row has none.
uint32_t SwatchOf(const nlohmann::json& row)
{
    const std::string hex = row.value("color", "");
    if (hex.size() != 7 || hex[0] != '#')
        return 0;
    return 0xFF000000u | static_cast<uint32_t>(std::strtoul(hex.c_str() + 1, nullptr, 16));
}
} // namespace

MarkupSearchProvider::MarkupSearchProvider(nlohmann::json rows)
    : m_Rows(std::move(rows))
{
}

void MarkupSearchProvider::BeginSearch(const std::string& query, ResultSink sink)
{
    m_LastQuery = query;
    std::map<std::string, std::size_t> titleCounts;
    std::map<std::string, std::size_t> tagCounts;
    for (const nlohmann::json& row : m_Rows)
    {
        ++titleCounts[row.value("title", "")];
        ++tagCounts[MarkupContext::TagOf(row)];
    }

    const std::string needle = ToLowerAscii(TrimWhitespace(query));
    std::vector<SearchResultItem> results;
    for (const nlohmann::json& row : m_Rows)
    {
        const std::string title = row.value("title", "");
        const std::string tag = MarkupContext::TagOf(row);
        if (!needle.empty() && ToLowerAscii(title).find(needle) == std::string::npos &&
            ToLowerAscii(tag).find(needle) == std::string::npos)
            continue;
        SearchResultItem item;
        const uint32_t entityId = row.value("entityId", 0u);
        item.Id = entityId;
        item.Label = !title.empty() ? title : !tag.empty() ? tag : std::string("Untitled mark-up");
        item.Detail = row.value("status", "") + kSeparator + row.value("kind", "");
        if (!tag.empty())
            item.Detail += kSeparator + std::string("tag ") + tag;
        item.Icon.SwatchArgb = SwatchOf(row);
        item.UserData = MarkupContext::NameOf({.EntityId = entityId,
                                               .Title = title,
                                               .Tag = tag,
                                               .SameTitle = titleCounts[title],
                                               .SameTag = tagCounts[tag]});
        results.push_back(std::move(item));
    }
    sink(std::move(results), true);
}

std::string MarkupSearchProvider::FormatResultSummary(std::span<const SearchResultItem> results) const
{
    if (results.empty())
        return "No mark-up's title or tag holds " + m_LastQuery;
    return std::to_string(results.size()) + (results.size() == 1 ? " mark-up" : " mark-ups");
}
} // namespace GameEngine
