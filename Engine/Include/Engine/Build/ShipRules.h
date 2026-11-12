#pragma once

#include "Types/FlatMap.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

class AssetRegistry;
struct AssetManifest;

/// Whether an asset ships in a game export.
enum class ShipRule : uint8_t
{
    Inferred, ///< Ships when a built scene or another shipped asset uses it.
    Always,   ///< Ships on every platform with what it uses, also when nothing uses it.
    Never,    ///< Never ships. The export fails when a shipped asset uses it.
};

/// The ship rules of a project's asset root. An asset's own rule is the kv key kAssetMetaKey in the
/// asset database. A `.assetfolder` file in a folder, {"ship": "always" | "never" | "inferred"}, sets
/// the rule for everything under that folder. The asset's own rule wins, then the nearest folder file
/// above it. With neither, the rule is Inferred.
class ShipRules
{
public:
    static constexpr std::string_view kFolderFileName = ".assetfolder";
    /// The asset database kv key of an asset's own rule: "always", "never" or "inferred".
    static constexpr std::string_view kAssetMetaKey = "assets.build.ship";

    /// What sets a resolved rule.
    enum class Source : uint8_t
    {
        Default, ///< No rule: Inferred.
        Asset,   ///< The asset's own kv rule.
        Folder,  ///< A folder file: the folder's own, or the nearest one above.
    };

    /// The rule for one path and what sets it.
    struct Resolved
    {
        ShipRule Rule = ShipRule::Inferred;
        Source From = Source::Default;
        /// For Source::Folder, the root-relative folder whose file sets the rule ("Data/"; "" for the asset root).
        std::string Folder;
    };

    /// Reads the folder files under `assetRoot`, outside the folders the asset ignore rules skip.
    /// A file that is not valid adds one sentence to `outErrors` that names it, and sets no rule.
    static ShipRules Load(const std::filesystem::path& assetRoot, std::vector<std::string>& outErrors);

    /// Reads only the folder files that apply to `relativePaths`: in each folder above a path, and in
    /// the path itself when it is a folder. For a view of a few assets, without a walk of the root.
    static ShipRules LoadAbove(const std::filesystem::path& assetRoot, std::span<const std::string> relativePaths,
                               std::vector<std::string>& outErrors);

    /// Adds the own rule of each asset that the store of `sourceAlias` (mounted at `assetRoot`) holds.
    /// A stored value that is not a rule, and Always on content that never ships, add a sentence to `outErrors`.
    void LoadAssetRules(const AssetRegistry& registry, std::string_view sourceAlias,
                        const std::filesystem::path& assetRoot, std::vector<std::string>& outErrors);

    /// The rule for `relativePath`, relative to the asset root the rules were loaded from: its own rule
    /// (an asset's kv rule, or a folder's own file), else the nearest folder file above it.
    Resolved Resolve(std::string_view relativePath) const;

    /// The own rule of `relativePath`: an asset's kv rule, or a folder's own file. Empty when it has none.
    std::optional<ShipRule> OwnRule(std::string_view relativePath) const;

    /// The rule `relativePath` gets from the folder files above it, without its own rule.
    Resolved ResolveInherited(std::string_view relativePath) const;

    /// True when no asset rule and no folder file sets a rule.
    bool IsEmpty() const { return m_Folders.Empty() && m_Assets.Empty(); }

    /// Adds one sentence to `outErrors` for each manifest entry whose source is under `assetRoot`
    /// and whose rule is Never: "<path> is set to Never ship (from its folder <folder>), but <consequence>"
    /// (DescribeNever).
    void ReportNeverEntries(const AssetManifest& manifest, const std::filesystem::path& assetRoot,
                            std::string_view consequence, std::vector<std::string>& outErrors) const;

    /// "<path> is set to Never ship (from its own rule)" for an asset's rule, or "(from <DescribeFolder>)" for a
    /// folder rule: the start of every Never error.
    static std::string DescribeNever(std::string_view relativePath, const Resolved& resolved);

    /// The folder file of a folder rule in words: "its folder Data/", or "the asset root's .assetfolder".
    static std::string DescribeFolder(const Resolved& resolved);

    /// The rule a stored text names ("always", "never", "inferred"); empty for any other text.
    static std::optional<ShipRule> Parse(std::string_view text);

    /// The stored text of `rule`: "always", "never" or "inferred".
    static std::string_view ToText(ShipRule rule);

    /// `file` relative to `assetRoot` in its spelling on disk, for messages; "" when it is outside the root.
    static std::string SpelledRelativePath(const std::filesystem::path& assetRoot, const std::filesystem::path& file);

private:
    // Reads the folder file of `folder` (canonical root-relative, "" for the root) when the folder has one.
    void ReadFolderFile(const std::filesystem::path& assetRoot, const std::string& folder,
                        std::vector<std::string>& outErrors);
    // Sets the own rule of the asset at `relativePath`.
    void SetAssetRule(std::string_view relativePath, ShipRule rule);

    FlatMap<std::string, Resolved> m_Folders; // Folded root-relative folder key ("" for the root).
    FlatMap<std::string, ShipRule> m_Assets;  // Folded root-relative asset key.
};

} // namespace GameEngine
