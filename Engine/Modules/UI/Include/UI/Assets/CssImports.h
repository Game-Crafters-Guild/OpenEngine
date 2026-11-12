#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {
class AssetManager;
}

namespace GameEngine::UI {

/// One top-level `@import "x.css";` / `@import url(x.css);` statement.
/// [Begin, End) covers the statement text including its terminating ';'.
struct CssImportStatement
{
    size_t Begin = 0;
    size_t End = 0;
    std::string Target;
};

/// Top-level @import statements in source order. Statements inside comments,
/// strings or rule blocks are ignored, as are statements with no target.
std::vector<CssImportStatement> FindTopLevelCssImports(std::string_view cssText);

/// Resolves an @import target: relative to the importing file first, then
/// through @p assets using the importer's source (empty when not found).
std::filesystem::path ResolveCssImportPath(const std::filesystem::path& importerPath,
                                           std::string_view rawImport,
                                           const AssetManager* assets);

} // namespace GameEngine::UI
