#include "Prefabs/PrefabBar.h"

#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabInstanceDescriptions.h"
#include "UI/ControlLayoutLoader.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/PrefabAuthoring.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

#include <array>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/PrefabBar/PrefabBar.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/PrefabBar/PrefabBar.css";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 3> kRequiredIds = {"prefab-bar-title", "prefab-bar-select-asset",
                                                          "prefab-bar-count"};

// The asset record of the prefab of the instance root `root`; false when the file is not in the project.
bool FindPrefabMetadata(ECS::World& world, ECS::EntityHandle root, AssetMetadata& out)
{
    const auto* instance = world.GetComponent<Components::PrefabInstance>(root);
    return instance &&
           EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(instance->Source, out);
}

// The prefab file of the instance root `root`; empty when the file is not in the project.
std::filesystem::path PrefabFileOf(ECS::World& world, ECS::EntityHandle root)
{
    AssetMetadata metadata;
    return FindPrefabMetadata(world, root, metadata) ? std::filesystem::path(metadata.Path) : std::filesystem::path();
}

std::string EntityNameOf(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    return name && !name->View().empty() ? std::string(name->View()) : std::string("Entity");
}

// The overrides of `entity` itself; empty outside every described instance.
std::vector<Scene::PrefabOverride> OwnOverridesOf(ECS::World& world, ECS::EntityHandle entity)
{
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(world, entity);
    return described ? described->Overrides : std::vector<Scene::PrefabOverride>();
}

// The first line for `entity`: "Instance of House" on an outermost instance root, "Added to House_01" on
// an entity the instance added, "Part of Porch in House_01" on any other entity of an instance, empty
// outside every instance.
std::string DescribeTitle(ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::EntityHandle nearest = FindInstanceRoot(world, entity);
    const ECS::EntityHandle outermost = FindOutermostInstanceRoot(world, entity);
    if (!nearest.IsValid())
        return {};
    const std::string prefabName = PrefabNameOf(world, nearest);
    const std::string shownPrefab = prefabName.empty() ? std::string("a missing prefab") : prefabName;
    if (entity == outermost)
        return "Instance of " + shownPrefab;
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(world, entity);
    if (described && described->AddedOnInstance)
        return "Added to " + EntityNameOf(world, outermost);
    return std::format("Part of {} in {}", shownPrefab, EntityNameOf(world, outermost));
}

// The number of overrides of the outermost instance that holds `entity`: each changed field, added or
// removed component, removed child and added entity.
std::size_t CountInstanceOverrides(ECS::World& world, ECS::EntityHandle entity)
{
    const DescribedPrefabInstance* instance = PrefabInstanceDescriptions::Get().FindInstance(world, entity);
    if (!instance)
        return 0;
    std::size_t count = 0;
    for (const Scene::PrefabInstanceEntity& described : instance->Entities)
        count += described.AddedOnInstance ? 1 : described.Overrides.size();
    return count;
}

// The count line and its tooltip.
struct CountLine
{
    std::string Text;
    std::string Tooltip;
};

// The count line. On the outermost root it counts the whole instance; on any other entity it counts the
// entity's own overrides, and its tooltip names the instance's count when only other entities have some.
// Empty on an entity the instance added: that entity is one of the root's overrides and has none of its own.
CountLine DescribeCount(ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::EntityHandle outermost = FindOutermostInstanceRoot(world, entity);
    const std::size_t total = CountInstanceOverrides(world, entity);
    if (entity == outermost)
        return {total == 0 ? std::string("No overrides") : FormatCount(total, "override", "overrides"), {}};
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(world, entity);
    if (described && described->AddedOnInstance)
        return {};
    const std::size_t own = described ? described->Overrides.size() : 0;
    if (own > 0)
        return {std::format("{} on {}", FormatCount(own, "override", "overrides"), EntityNameOf(world, entity)), {}};
    if (total > 0)
        return {"No overrides on " + EntityNameOf(world, entity),
                std::format("{} on {}", FormatCount(total, "override", "overrides"), EntityNameOf(world, outermost))};
    return {"No overrides", {}};
}

// The components the overrides `overrides` remove from the entity; the Inspector shows a header for each.
std::vector<std::string> RemovedComponentsOf(const std::vector<Scene::PrefabOverride>& overrides)
{
    std::vector<std::string> removed;
    for (const Scene::PrefabOverride& entry : overrides)
        if (entry.Kind == Scene::PrefabOverrideKind::RemovedComponent)
            removed.push_back(entry.Component);
    return removed;
}

} // namespace

PrefabBar::PrefabBar(ECS::World& world, ECS::EntityHandle entity, EditorChangeNotifications* notifications,
                     SelectAssetHandler selectAsset, std::function<void()> refreshMarks,
                     std::function<void()> requestRebuild)
    : m_World(world), m_Entity(entity), m_Notifications(notifications), m_SelectAsset(std::move(selectAsset)),
      m_RefreshMarks(std::move(refreshMarks)), m_RequestRebuild(std::move(requestRebuild)),
      m_ShownOverrides(OwnOverridesOf(world, entity))
{
    AddClass("prefab-bar-host");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    std::unique_ptr<UIElement> layout = ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!layout)
        return;
    m_Title = dynamic_cast<Label*>(layout->FindById("prefab-bar-title"_sid));
    m_Count = dynamic_cast<Label*>(layout->FindById("prefab-bar-count"_sid));
    m_SelectAssetButton = dynamic_cast<Button*>(layout->FindById("prefab-bar-select-asset"_sid));
    AddChild(std::move(layout));
    if (m_SelectAssetButton)
        m_SelectAssetButton->SetOnClick([this](UIEvent&) { SelectAsset(); });
    if (m_Notifications)
    {
        m_ComponentToken = m_Notifications->SubscribeComponentChanged(
            [this](const EditorChangeNotifications::ComponentChangedEvent& change) { OnComponentChanged(change); });
        m_StructureToken = m_Notifications->SubscribeWorldStructureChanged(
            [this](const EditorChangeNotifications::WorldStructureChangedEvent& change)
            { OnWorldStructureChanged(change); });
    }
    Refresh();
}

PrefabBar::~PrefabBar()
{
    if (!m_Notifications)
        return;
    m_Notifications->Unsubscribe(m_ComponentToken);
    m_Notifications->Unsubscribe(m_StructureToken);
}

void PrefabBar::Refresh()
{
    if (!m_Title || !m_Count || !m_World.IsValid(m_Entity))
        return;
    m_Root = FindOutermostInstanceRoot(m_World, m_Entity);
    m_Title->SetText(DescribeTitle(m_World, m_Entity));
    const CountLine count = DescribeCount(m_World, m_Entity);
    m_Count->SetText(count.Text);
    m_Count->SetTooltip(count.Tooltip);
    if (count.Text.empty())
        m_Count->AddClass("hidden");
    else
        m_Count->RemoveClass("hidden");
    if (!m_SelectAssetButton)
        return;
    const ECS::EntityHandle nearest = FindInstanceRoot(m_World, m_Entity);
    const std::string prefabName = PrefabNameOf(m_World, nearest);
    const std::filesystem::path file = PrefabFileOf(m_World, nearest);
    m_SelectAssetButton->SetText(prefabName.empty() ? std::string("Select Prefab") : "Select " + prefabName);
    m_SelectAssetButton->SetTooltip(file.empty() ? std::string("The prefab file is not in the project.")
                                                 : std::format("Show {} in the Assets panel.", file.filename().string()));
    m_SelectAssetButton->SetEnabled(!file.empty());
}

void PrefabBar::RefreshAfterChange()
{
    if (!m_World.IsValid(m_Entity))
        return;
    PrefabInstanceDescriptions::Get().Forget(m_World, m_Entity);
    Refresh();
    std::vector<Scene::PrefabOverride> overrides = OwnOverridesOf(m_World, m_Entity);
    const bool removedChanged = RemovedComponentsOf(overrides) != RemovedComponentsOf(m_ShownOverrides);
    const bool overridesChanged = overrides != m_ShownOverrides;
    m_ShownOverrides = std::move(overrides);
    if (overridesChanged && m_RefreshMarks)
        m_RefreshMarks();
    // Only a rebuild drops the bar of an entity that left its instance or changes the removed headers.
    const bool leftInstance = !FindInstanceRoot(m_World, m_Entity).IsValid();
    if (m_RequestRebuild && (leftInstance || removedChanged))
        m_RequestRebuild();
}

void PrefabBar::SelectAsset()
{
    const std::filesystem::path file = PrefabFileOf(m_World, FindInstanceRoot(m_World, m_Entity));
    if (!file.empty() && m_SelectAsset)
        m_SelectAsset(file);
}

void PrefabBar::OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& change)
{
    // A drag reports each step; the bar reads the world again at the commit.
    using Kind = EditorChangeNotifications::ChangeKind;
    if (change.world != &m_World || (change.kind != Kind::Commit && change.kind != Kind::UndoRedo))
        return;
    // Only a commit inside the shown instance can change what the bar reads.
    if (m_World.IsValid(change.entity) && FindOutermostInstanceRoot(m_World, change.entity) != m_Root)
        return;
    RefreshAfterChange();
}

void PrefabBar::OnWorldStructureChanged(const EditorChangeNotifications::WorldStructureChangedEvent& change)
{
    if (change.world == &m_World)
        RefreshAfterChange();
}

} // namespace GameEngine::Editor
