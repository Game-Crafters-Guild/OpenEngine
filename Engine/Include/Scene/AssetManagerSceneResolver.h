#pragma once

#include "Scene/SceneAssetResolver.h"

#include <filesystem>

namespace GameEngine
{
class AssetManager;
class AssetRegistry;
} // namespace GameEngine

namespace GameEngine::Scene
{

/// The scene asset resolver backed by the engine's AssetManager and its registry: paths resolve
/// through the mounts, GUIDs through the registry's redirects.
class AssetManagerSceneResolver final : public ISceneAssetResolver
{
  public:
    /// What GetOrCreateAssetGuid does with a file the registry does not know.
    enum class UnknownFile
    {
        Register,  ///< registers it as an asset (the editor: the file is then listed and watched)
        AssignGuid ///< derives a GUID for it without listing it (the runtime)
    };

    AssetManagerSceneResolver(AssetManager& assetManager, std::filesystem::path assetRoot, UnknownFile unknownFile);

    std::filesystem::path GetAssetRoot() const override { return m_AssetRoot; }
    std::filesystem::path ResolveAssetPath(const std::filesystem::path& authoredPath) const override;
    GUID ResolveGuid(const GUID& guid) const override;
    GUID GetOrCreateAssetGuid(const std::filesystem::path& absolutePath) override;
    bool TryGetPathAndType(const GUID& guid, std::filesystem::path& outPath, AssetType& outType) const override;
    bool TryGetGuidAndType(const std::filesystem::path& absolutePath, GUID& outGuid, AssetType& outType) const override;

  private:
    AssetManager& m_AssetManager;
    AssetRegistry& m_Registry;
    std::filesystem::path m_AssetRoot;
    UnknownFile m_UnknownFile;
};

} // namespace GameEngine::Scene
