#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine::Editor
{

// SettingsStore-backed default raster size for SVG texture assets without a
// per-asset override. The value belongs to the active project. Editor UI SVGs
// are not affected: the UI rasterizes them at the size they are drawn at.
class SvgRasterSettings
{
  public:
    static constexpr uint32_t kDefaultSize = 256u;

    static uint32_t Load(const std::filesystem::path& workspaceRoot,
                         std::string* outError = nullptr);

    // Persists and applies a new default. When reloadAssets is true, loaded SVG
    // assets in the project sources are scheduled for reload.
    static bool SaveAndApply(const std::filesystem::path& workspaceRoot,
                             uint32_t pixels,
                             bool reloadAssets = true,
                             std::string* outError = nullptr);

    // Applies the persisted default without rewriting the settings file.
    // Used before editor assets load and whenever the active project changes.
    static void ApplySavedDefault(const std::filesystem::path& workspaceRoot);
};

} // namespace GameEngine::Editor
