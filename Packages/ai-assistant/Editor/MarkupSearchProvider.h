#pragma once

#include "MarkupContext.h"

#include "UI/Controls/SearchDialog.h"

#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine
{
/// The `@` picker's rows: the scene's mark-ups as markup_list returns them, newest change first.
/// A row reads its title (its tag for an untitled one, "Untitled mark-up" for one with neither)
/// over its status, its kind and its tag (MarkupContext::TagOf), with a swatch in the color the
/// mark-up draws in. A query keeps the rows whose title or tag holds it (ignoring ASCII case).
/// Choosing a row puts in the token MarkupContext::NameOf gives it; a row no token names apart
/// carries the refusal (SearchResultItem::UserData, a MarkupPick).
class MarkupSearchProvider final : public ISearchProvider
{
public:
    /// `rows` is markup_list's "markups" array.
    explicit MarkupSearchProvider(nlohmann::json rows);

    void BeginSearch(const std::string& query, ResultSink sink) override;
    void CancelSearch() override {}
    std::string FormatResultSummary(std::span<const SearchResultItem> results) const override;
    std::string GetPlaceholderText() const override { return "Search mark-ups by title or tag"; }

    /// What the user typed in the list's search field last.
    const std::string& LastQuery() const { return m_LastQuery; }

private:
    nlohmann::json m_Rows;
    std::string m_LastQuery;
};
} // namespace GameEngine
