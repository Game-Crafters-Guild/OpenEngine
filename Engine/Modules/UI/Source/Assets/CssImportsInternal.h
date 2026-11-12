#pragma once

#include "UI/Assets/CssImports.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::UI {

/// Absolute, lexically normal, case-folded on Windows; identifies a stylesheet
/// file for @import cycle detection and stylesheet caches.
std::string NormalizeStylesheetPathKey(const std::filesystem::path& path);

/// Inlines every @import of @p cssText recursively. Unresolvable, unreadable
/// and cyclic imports are dropped with a warning. @p outImportFiles receives
/// every transitively imported file.
void ExpandCssImports(const std::filesystem::path& importerPath,
                      std::string_view cssText,
                      const AssetManager* assets,
                      std::string& outExpanded,
                      std::vector<std::filesystem::path>& outImportFiles);

/// The file list of ExpandCssImports without building the expanded text.
void CollectCssImportFiles(const std::filesystem::path& importerPath,
                           std::string_view cssText,
                           const AssetManager* assets,
                           std::vector<std::filesystem::path>& outImportFiles);

} // namespace GameEngine::UI
