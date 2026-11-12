#pragma once

#include "Assets/ParserRegistry.h"
#include "UI/Assets/UIStyleAsset.h"

namespace GameEngine
{

// UI Style Asset Parser - handles .css, .uss files
class UIStyleAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::UIStyle; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".css", ".uss"}; }

    // A sheet the UI module ships (UI::kControlAssetPaths) gets StyleOrigin::Control; every
    // other sheet gets StyleOrigin::Author.
    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override;

    std::string GetName() const override { return "UIStyleAssetParser"; }

    int GetPriority() const override { return 100; }

    // Emits a UIElementImage edge per `url(...)` outside an @import, mount prefix ("editor:", "@editor/") stripped
    // so the edge resolves through ResolvePathTarget, and a UIStyleImport edge per top-level @import,
    // to the file UI::ResolveCssImportPath resolves, as the runtime does.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
