#pragma once

#include "Assets/NavSurfaceAsset.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

class NavSurfaceAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::NavigationSurfaceData; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".navsurface"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        return AssetParseResult(std::make_shared<NavSurfaceAsset>(metadata.Guid, metadata.Path));
    }

    std::string GetName() const override { return "NavSurfaceAssetParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
