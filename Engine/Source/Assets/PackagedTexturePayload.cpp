#include "Assets/PackagedTexturePayload.h"

#include "AssetDatabase/AssetDatabasePaths.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"

#include <string>

namespace GameEngine
{
namespace
{
bool IsPayloadFilename(const std::string& filename)
{
    return !filename.empty() && filename.find_first_of("/\\:") == std::string::npos &&
           std::filesystem::path(filename).extension() == ".ktx2";
}
}

std::optional<std::filesystem::path> ResolvePackagedTexturePayload(const AssetRegistry& registry, const GUID& guid)
{
    AssetMetadata metadata;
    if (registry.GetDerivedArtifactPolicy(guid).CooksOnMiss ||
        !registry.TryGetAssetMetadata(guid, metadata))
        return std::nullopt;
    std::string payload;
    if (!registry.TryGetMetaValue(metadata.Path, kTexturePackagedMetaKey, payload))
        return std::nullopt;
    const auto root = registry.TryGetCacheRoot(guid);
    if (!root || !IsPayloadFilename(payload))
    {
        Logger::Log::Error("TextureAsset[{}]: invalid packaged payload; rebuild the package", metadata.Path.string());
        return std::filesystem::path{};
    }
    return *root / AssetDatabase::kTextureCacheDirectoryName / payload;
}
} // namespace GameEngine
