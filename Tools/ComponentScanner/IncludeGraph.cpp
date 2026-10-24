#include "IncludeGraph.h"

#include "CommentStripper.h"
#include "Lexical.h"
#include "SourceFiles.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <utility>

namespace GameEngine::ComponentScanner
{
namespace
{

namespace fs = std::filesystem;

// The target of an `#include "..."` or `#include <...>` line, or nullopt for any other line.
std::optional<std::string> IncludeTarget(std::string_view line)
{
    std::string_view text = Lexical::TrimView(line);
    if (!Lexical::StartsWith(text, "#"))
        return std::nullopt;
    text = Lexical::TrimView(text.substr(1));
    if (!Lexical::StartsWith(text, "include"))
        return std::nullopt;
    text = Lexical::TrimView(text.substr(std::string_view("include").size()));
    if (text.empty() || (text.front() != '"' && text.front() != '<'))
        return std::nullopt;
    const char close = text.front() == '"' ? '"' : '>';
    const std::size_t end = text.find(close, 1);
    if (end == std::string_view::npos || end == 1)
        return std::nullopt;
    return std::string(text.substr(1, end - 1));
}

} // namespace

IncludeGraph::IncludeGraph(std::vector<fs::path> includeRoots)
{
    for (const fs::path& root : includeRoots)
    {
        m_IncludeRoots.push_back(FullPath(root));
        std::set<std::string>& entries = m_RootEntries.emplace_back();
        std::error_code ec;
        for (fs::directory_iterator it(m_IncludeRoots.back(), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            entries.insert(Utf8(it->path().filename()));
    }
}

std::vector<const IncludedHeader*> IncludeGraph::Closure(const fs::path& file, const std::vector<std::string>& lines)
{
    const fs::path start = FullPath(file);
    std::vector<const IncludedHeader*> closure;
    std::set<const Node*> visited;
    std::vector<Node*> pending = Resolve(start.parent_path(), lines);
    std::reverse(pending.begin(), pending.end());
    while (!pending.empty())
    {
        Node* node = pending.back();
        pending.pop_back();
        if (node->Header.File == start || !visited.insert(node).second)
            continue;
        closure.push_back(&node->Header);
        if (!node->Linked)
        {
            node->Includes = Resolve(node->Header.File.parent_path(), node->Header.Lines);
            node->Linked = true;
        }
        for (auto it = node->Includes.rbegin(); it != node->Includes.rend(); ++it)
            pending.push_back(*it);
    }
    return closure;
}

std::vector<IncludeGraph::Node*> IncludeGraph::Resolve(const fs::path& folder, const std::vector<std::string>& lines)
{
    std::vector<Node*> includes;
    for (const std::string& line : lines)
    {
        const std::optional<std::string> target = IncludeTarget(line);
        if (!target)
            continue;
        if (const std::optional<fs::path> file = Find(*target, folder))
            if (Node* node = Read(*file, *target))
                includes.push_back(node);
    }
    return includes;
}

std::optional<fs::path> IncludeGraph::Find(const std::string& target, const fs::path& folder)
{
    std::error_code ec;
    if (const fs::path beside = folder / PathFromUtf8(target); fs::is_regular_file(beside, ec))
        return FullPath(beside);
    const auto [it, inserted] = m_UnderRoots.try_emplace(target);
    if (inserted)
        it->second = FindUnderRoots(target);
    return it->second;
}

std::optional<fs::path> IncludeGraph::FindUnderRoots(const std::string& target) const
{
    const std::string firstSegment = target.substr(0, target.find('/'));
    const fs::path relative = PathFromUtf8(target);
    std::error_code ec;
    for (std::size_t i = 0; i < m_IncludeRoots.size(); ++i)
        if (m_RootEntries[i].count(firstSegment) != 0)
            if (const fs::path candidate = m_IncludeRoots[i] / relative; fs::is_regular_file(candidate, ec))
                return FullPath(candidate);
    return std::nullopt;
}

IncludeGraph::Node* IncludeGraph::Read(const fs::path& file, const std::string& target)
{
    const auto [it, inserted] = m_Nodes.try_emplace(file);
    if (!inserted)
        return it->second.get();
    std::string raw;
    if (!ReadText(file, raw))
        return nullptr;
    auto node = std::make_unique<Node>();
    node->Header.File = file;
    node->Header.IncludePath = IncludePathOf(file, target);
    node->Header.Lines = SplitLines(raw);
    node->Header.Stripped = CommentStripper::Strip(raw).Text;
    it->second = std::move(node);
    return it->second.get();
}

std::string IncludeGraph::IncludePathOf(const fs::path& file, const std::string& target) const
{
    for (const fs::path& root : m_IncludeRoots)
    {
        const fs::path relative = file.lexically_relative(root);
        if (!relative.empty() && *relative.begin() != "..")
            return Utf8(relative);
    }
    return target;
}

} // namespace GameEngine::ComponentScanner
