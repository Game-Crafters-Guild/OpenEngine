#include "Scene/AssetManagerSceneResolver.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"

#include <utility>

namespace GameEngine::Scene
{

AssetManagerSceneResolver::AssetManagerSceneResolver(AssetManager& assetManager, std::filesystem::path assetRoot,
                                                     UnknownFile unknownFile)
    : m_AssetManager(assetManager), m_Registry(assetManager.GetRegistry()), m_AssetRoot(std::move(assetRoot)),
      m_UnknownFile(unknownFile)
{
}

std::filesystem::path AssetManagerSceneResolver::ResolveAssetPath(const std::filesystem::path& authoredPath) const
{
    return m_AssetManager.ResolveAssetPath(authoredPath);
}

GUID AssetManagerSceneResolver::ResolveGuid(const GUID& guid) const
{
    return m_Registry.ResolveGuid(guid);
}

GUID AssetManagerSceneResolver::GetOrCreateAssetGuid(const std::filesystem::path& absolutePath)
{
    if (m_UnknownFile == UnknownFile::AssignGuid)
        return m_Registry.GetOrCreateAssetGUID(absolutePath);
    const auto result = m_Registry.RegisterAssetByPath(absolutePath);
    return result.IsOk() ? result.Value() : GUID::Null();
}

bool AssetManagerSceneResolver::TryGetPathAndType(const GUID& guid, std::filesystem::path& outPath,
                                                  AssetType& outType) const
{
    AssetMetadata metadata{};
    if (!m_Registry.TryGetAssetMetadata(guid, metadata))
        return false;
    outPath = metadata.Path;
    outType = metadata.Type;
    return true;
}

bool AssetManagerSceneResolver::TryGetGuidAndType(const std::filesystem::path& absolutePath, GUID& outGuid,
                                                  AssetType& outType) const
{
    AssetMetadata metadata{};
    if (!m_Registry.TryGetAssetMetadata(absolutePath, metadata))
        return false;
    outGuid = metadata.Guid;
    outType = metadata.Type;
    return true;
}

} // namespace GameEngine::Scene
