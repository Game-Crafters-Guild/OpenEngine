#include "Assets/Parsers/TextureParser.h"

#include "Assets/AssetManager.h"
#include "Assets/PackagedTexturePayload.h"
#include "Assets/TextureAsset.h"

namespace GameEngine
{
AssetParseResult TextureParser::Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager)
{
    return AssetParseResult(std::make_shared<TextureAsset>(metadata.Guid, metadata.Path));
}

std::filesystem::path TextureParser::ResolveReadPath(
    const AssetMetadata& metadata, const AssetManager& assetManager) const
{
    return ResolvePackagedTexturePayload(assetManager.GetRegistry(), metadata.Guid).value_or(metadata.Path);
}
} // namespace GameEngine
