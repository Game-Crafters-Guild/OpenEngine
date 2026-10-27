#pragma once

#include "EditorChangeNotifications.h"
#include "UI/UIElement.h"

#include "ECS/ECS.h"
#include "Scene/PrefabAuthoring.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace GameEngine
{
class Button;
class Label;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

/// The Inspector's bar on an entity of a prefab instance: the prefab and the instance on line 1, the
/// button that selects the prefab file and the override count on line 2. A commit in its instance
/// refreshes it and the Inspector's override marks in place; only an exit from the instance or a
/// change to the removed components asks for a rebuild.
class PrefabBar final : public UIElement
{
  public:
    /// Shows the prefab file at `prefabFile` in the Assets panel.
    using SelectAssetHandler = std::function<void(const std::filesystem::path& prefabFile)>;

    /// A bar for `entity`, which is in a prefab instance of `world`. `notifications` may be null: the
    /// bar then shows the state at construction. `refreshMarks` sets the Inspector's override marks
    /// again in place; `requestRebuild` rebuilds the Inspector later.
    PrefabBar(ECS::World& world, ECS::EntityHandle entity, EditorChangeNotifications* notifications,
              SelectAssetHandler selectAsset, std::function<void()> refreshMarks, std::function<void()> requestRebuild);
    ~PrefabBar() override;

    PrefabBar(const PrefabBar&) = delete;
    PrefabBar& operator=(const PrefabBar&) = delete;

  private:
    void Refresh();
    void RefreshAfterChange();
    void SelectAsset();
    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& change);
    void OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& change);

    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    // The outermost instance root of the entity at the last refresh.
    ECS::EntityHandle m_Root;
    EditorChangeNotifications* m_Notifications = nullptr;
    EditorChangeNotifications::SubscriptionToken m_ComponentToken{};
    EditorChangeNotifications::SubscriptionToken m_StructureToken{};
    SelectAssetHandler m_SelectAsset;
    std::function<void()> m_RefreshMarks;
    std::function<void()> m_RequestRebuild;
    // The entity's own overrides as the Inspector's marks show them.
    std::vector<Scene::PrefabOverride> m_ShownOverrides;
    Label* m_Title = nullptr;
    Label* m_Count = nullptr;
    Button* m_SelectAssetButton = nullptr;
};

} // namespace GameEngine::Editor
