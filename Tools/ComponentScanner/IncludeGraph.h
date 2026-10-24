#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// A header reached through the includes of a component header: its absolute path, its path
// relative to the include root it sits under (else the include as written), its raw lines and
// its text with the comments removed.
struct IncludedHeader
{
    std::filesystem::path File;
    std::string IncludePath;
    std::vector<std::string> Lines;
    std::string Stripped;
};

// The include graph of the component headers, read as it is walked. A quoted or angled
// `#include` is looked up beside the including header, then under each include root in order;
// one found nowhere (a standard or third-party header) is not followed. Each header is read,
// stripped and its includes looked up once, whichever closure reaches it first, so the
// closures of many component headers cost one read of the headers they share. An include is
// probed only under the roots whose top level holds its first path segment, and its lookup
// under the roots is remembered, so a walk over many roots costs a few file probes per include.
class IncludeGraph
{
  public:
    explicit IncludeGraph(std::vector<std::filesystem::path> includeRoots);

    // The headers `file` includes, transitively, each once, in the order first reached; `file`
    // itself is left out. `lines` are its raw lines, so a component header is not read twice.
    std::vector<const IncludedHeader*> Closure(const std::filesystem::path& file, const std::vector<std::string>& lines);

  private:
    struct Node
    {
        IncludedHeader Header;
        std::vector<Node*> Includes;
        bool Linked = false;
    };

    std::vector<Node*> Resolve(const std::filesystem::path& folder, const std::vector<std::string>& lines);
    std::optional<std::filesystem::path> Find(const std::string& target, const std::filesystem::path& folder);
    std::optional<std::filesystem::path> FindUnderRoots(const std::string& target) const;
    Node* Read(const std::filesystem::path& file, const std::string& target);
    std::string IncludePathOf(const std::filesystem::path& file, const std::string& target) const;

    std::vector<std::filesystem::path> m_IncludeRoots;
    // The names at the top level of each include root, by root index.
    std::vector<std::set<std::string>> m_RootEntries;
    std::map<std::string, std::optional<std::filesystem::path>> m_UnderRoots;
    std::map<std::filesystem::path, std::unique_ptr<Node>> m_Nodes;
};

} // namespace GameEngine::ComponentScanner
