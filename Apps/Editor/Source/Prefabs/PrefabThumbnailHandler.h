#pragma once

#include "Thumbnails/AssetThumbnailHandler.h"

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine::Editor
{

/// The Assets panel's tile for a `.prefab` file: the prefab glyph, the teal cube that also marks
/// an instance in the Hierarchy. The glyph is the same for every prefab; no render.
class PrefabThumbnailHandler final : public AssetThumbnailHandler
{
  public:
    /// The glyph, a path in the editor's assets.
    static constexpr const char* kGlyphPath = "Icons/PrefabAsset.svg";

    std::string GetOrRequest(const std::filesystem::path& assetPath,
                             int desiredSize,
                             std::function<void(const std::string& relPath)> onReady,
                             bool staticModelListThumbnail = false) override;
};

} // namespace GameEngine::Editor
