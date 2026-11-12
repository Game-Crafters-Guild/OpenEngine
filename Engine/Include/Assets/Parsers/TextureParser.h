#pragma once

#include "Assets/ParserRegistry.h"

namespace GameEngine
{
/// Shared texture construction and packaged-payload selection for raster/container parsers.
class TextureParser : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::Texture; }
    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager& assetManager) override;
    std::filesystem::path ResolveReadPath(const AssetMetadata& metadata, const AssetManager& assetManager) const override;
    /// The cooked artifacts the decode adopts, with the cook plan that names them
    /// (TextureAsset::ResolveCookDecodeInputs); for a packaged payload, its normal-variance
    /// companion (TextureAsset::ResolvePackagedDecodeInputs).
    AssetDecodeInputs ResolveDecodeInputs(const AssetMetadata& metadata, std::span<const uint8> bytes,
                                          AssetManager& assetManager) const override;
    /// The cooked artifacts of a cook hit, named before the source is read
    /// (TextureAsset::ResolveCookHitDecodeInputs); empty for a packaged payload.
    AssetDecodeInputs ResolveDecodeInputsWithoutBytes(const AssetMetadata& metadata,
                                                      AssetManager& assetManager) const override;
};
} // namespace GameEngine
