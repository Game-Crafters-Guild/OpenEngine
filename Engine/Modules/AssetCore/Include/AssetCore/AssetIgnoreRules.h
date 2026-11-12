#pragma once

#include "Types/FlatSet.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

/**
 * Asset ignore rules used by the asset database + scanners.
 *
 * Goals:
 * - Prevent indexing of derived/build/tool folders (performance + correctness).
 * - Allow per-project overrides via <AssetRoot>/.assetignore (git-tracked).
 *
 * Rule support (minimal, fast):
 * - Comments: lines starting with '#'
 * - Blank lines ignored
 * - Directory name ignore: "DirName/" (matches any directory segment with that name)
 * - Prefix ignore: "Some/Prefix/**" (matches any path under that prefix)
 * - Extension ignore: "*.ext" (matches file extension)
 */
struct AssetIgnoreRules
{
    // Lower-cased directory names that should never be recursed into / indexed.
    std::unordered_set<std::string> ignoredDirNamesLower;
    // Lower-cased directory-name prefixes: a whole path segment that starts with
    // one of these is ignored. Build trees that name themselves after the
    // configuration they hold ("cmake-build-debug") need it; a plain name cannot
    // cover them. Defaults only — .assetignore has no syntax for a name prefix.
    std::vector<std::string> ignoredDirNamePrefixesLower;
    // Lower-cased file basenames that should never be indexed.
    std::unordered_set<std::string> ignoredFileNamesLower;
    // Lower-cased extensions (including dot) that should never be indexed.
    std::unordered_set<std::string> ignoredExtensionsLower;
    // Lower-cased canonical prefixes (forward slashes), always ending with '/'.
    std::vector<std::string> ignoredPrefixesLower;
    // Lower-cased names of tool output folders ("build", "Library"). Directly under the
    // root, the defaults ignore them by path prefix. Deeper, a folder with one of these
    // names is ignored only when it holds a file of toolOutputMarkerFileNames.
    FlatSet<std::string> toolOutputDirNamesLower;
    // File names that mark a folder as tool output (a CMake build tree, a Python environment).
    std::vector<std::string> toolOutputMarkerFileNames;

    // A folder that the rules ignore, and the rule in words for a log line.
    struct IgnoredFolder
    {
        std::filesystem::path Folder;
        std::string Reason;
    };

    static AssetIgnoreRules CreateDefault();

    // Loads defaults + optional <assetRoot>/.assetignore.
    static AssetIgnoreRules LoadForAssetRoot(const std::filesystem::path& assetRoot);

    // Adds a package's top-level Tests folder (any case) as an ignored path prefix
    // for a walk rooted at `sourceRoot`, when that folder lies under it. Its sources
    // build into a test executable the engine's own build defines, never into a
    // module the package delivers, so package code discovery, the generated module
    // build and its staleness digest all apply this one rule. No-op for an empty
    // `packageRoot` (a project's own scripts) or a `sourceRoot` beside the folder.
    void IgnorePackageTestsFolder(const std::filesystem::path& sourceRoot, const std::filesystem::path& packageRoot);

    // Tests a canonical relative path (forward slashes, no leading slash).
    bool ShouldIgnoreCanonicalRelativePath(std::string_view canonicalRel) const;

    // Tests a directory path for skipping recursion (expects an absolute or root-relative path).
    bool ShouldIgnoreDirectory(const std::filesystem::path& directoryPath,
                               const std::filesystem::path& assetRoot) const;

    // Tests a file path for indexing (expects an absolute or root-relative path).
    bool ShouldIgnoreFile(const std::filesystem::path& filePath,
                          const std::filesystem::path& assetRoot) const;

    // Returns the outermost folder of a canonical relative file path that a folder rule
    // ignores, or nothing. Reads the disk for the tool output markers. A hidden folder
    // (a name that starts with '.') returns nothing: it is not content.
    std::optional<IgnoredFolder> FindIgnoredFolder(std::string_view canonicalRel,
                                                   const std::filesystem::path& assetRoot) const;

    // Stable 64-bit hash of all rule sets, deterministic across runs and
    // platforms. Used by the warm-start snapshot to invalidate cached
    // records when .assetignore changes (a file that was previously
    // ignored may now be visible, and vice versa). Zero is reserved for
    // "no signature recorded".
    uint64_t Signature() const;
};

} // namespace GameEngine
