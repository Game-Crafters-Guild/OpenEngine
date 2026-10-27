#include "Prefabs/PrefabService.h"

#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "SceneView/SceneViewNoticeOverlay.h"

#include "Assets/AssetManager.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/PrefabAuthoring.h"

#include <format>
#include <span>

namespace GameEngine::Editor
{

PrefabService::PrefabService(AssetManager& assets)
    : m_Assets(assets)
{
}

ECS::EntityHandle PrefabService::Place(ECS::World& world, const std::filesystem::path& prefabFile,
                                       const Components::Transform& at, ECS::EntityHandle parent)
{
    const std::string fileName = prefabFile.filename().string();
    const GUID prefab = m_Assets.ResolveAssetGuid(prefabFile);
    if (prefab.IsNull())
    {
        Logger::Log::Warning("Prefabs: {} is not in the asset database; place it again once the Assets panel lists it",
                             prefabFile.string());
        ShowSceneViewNotice(std::format("{} could not be placed. Place it again once the Assets panel lists it.",
                                        fileName));
        return {};
    }

    const ECS::EntityHandle root = Scene::PlacePrefab(world, prefab, at, parent);
    if (!root.IsValid())
    {
        ShowSceneViewNotice(std::format("{} could not be placed. The log names the cause and the fix.", fileName));
        return {};
    }
    if (!parent.IsValid())
        AppendUnorderedRootsToHierarchyEnd(world, std::span(&root, 1));
    return root;
}

} // namespace GameEngine::Editor
