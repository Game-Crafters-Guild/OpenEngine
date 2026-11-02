#include "Prefabs/PrefabBar.h"

#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabInstanceDescriptions.h"
#include "Prefabs/PrefabOverridesList/PrefabOverridesList.h"
#include "Prefabs/PrefabOverridesList/PrefabOverridesListModel.h"
#include "Scene/SceneDocumentManager.h"
#include "UI/ControlLayoutLoader.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Name.h"
#include "Components/PrefabInstance.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Scene/PrefabAuthoring.h"
#include "Scene/SceneIO.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

#include <array>
#include <format>
#include <memory>
#include <span>
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
constexpr std::array<std::string_view, 14> kRequiredIds = {
    "prefab-bar-title",       "prefab-bar-glyph",          "prefab-bar-title-lead",
    "prefab-bar-prefab",      "prefab-bar-title-tail",     "prefab-bar-select-asset",
    "prefab-bar-counts",      "prefab-bar-count-none",     "prefab-bar-count",
    "prefab-bar-count-text",  "prefab-bar-instance-count", "prefab-bar-instance-count-text",
    "prefab-bar-stale-count", "prefab-bar-stale-count-text"};
// The glyph shows the Hierarchy's instance glyph, or the missing glyph when the prefab file is not in the
// project; the name takes its missing class then.
constexpr const char* kPrefabGlyphClass = "hierarchy-entity-prefab";
constexpr const char* kMissingGlyphClass = "prefab-bar-glyph-missing";
constexpr const char* kMissingNameClass = "prefab-bar-prefab-missing";
// A pill's button has this class while the pointer is over it (controls/Button.css .badge-button.hovered).
constexpr const char* kPillHoveredClass = "hovered";

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

// The first line in three phrases, so the prefab's name takes its own color.
struct TitleParts
{
    std::string Lead;
    std::string Prefab;
    std::string Tail;
    // True when the prefab file is not in the project.
    bool PrefabMissing = false;
};

// The first line for `entity`: "Instance of House" on an outermost instance root, "Added to House_01" on
// an entity the instance added, "Part of Porch in House_01" on any other entity of an instance, empty
// outside every instance.
TitleParts DescribeTitle(ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::EntityHandle nearest = FindInstanceRoot(world, entity);
    const ECS::EntityHandle outermost = FindOutermostInstanceRoot(world, entity);
    if (!nearest.IsValid())
        return {};
    const std::string prefabName = PrefabNameOf(world, nearest);
    const bool missing = prefabName.empty();
    const std::string shownPrefab = missing ? std::string("a missing prefab") : prefabName;
    if (entity == outermost)
        return {"Instance of", shownPrefab, {}, missing};
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(world, entity);
    if (described && described->AddedOnInstance)
        return {"Added to", {}, EntityNameOf(world, outermost), false};
    return {"Part of", shownPrefab, "in " + EntityNameOf(world, outermost), missing};
}

// The count lines: the entity's line, and the instance's line under it. A line with no override is plain text.
struct CountLines
{
    std::string Entity;
    bool EntityHasNone = false;
    std::string Instance;
};

// On the outermost root the first line counts the whole instance (`total`). On any other entity it counts the
// entity's own overrides, and the second line counts the instance's when only other entities have some.
// Both are empty on an entity the instance added: it is one of the root's overrides and has none of its own.
CountLines DescribeCounts(ECS::World& world, ECS::EntityHandle entity, std::size_t total)
{
    const ECS::EntityHandle outermost = FindOutermostInstanceRoot(world, entity);
    if (entity == outermost)
        return total == 0 ? CountLines{"No overrides", true, {}}
                          : CountLines{FormatCount(total, "override", "overrides"), false, {}};
    const Scene::PrefabInstanceEntity* described = PrefabInstanceDescriptions::Get().FindEntity(world, entity);
    if (described && described->AddedOnInstance)
        return {};
    const std::size_t own = described ? described->Overrides.size() : 0;
    if (own > 0)
        return {std::format("{} on {}", FormatCount(own, "override", "overrides"), EntityNameOf(world, entity)),
                false,
                {}};
    if (total > 0)
        return {"No overrides on " + EntityNameOf(world, entity), true,
                std::format("{} on {}", FormatCount(total, "override", "overrides"), EntityNameOf(world, outermost))};
    return {"No overrides", true, {}};
}

// Sets the pill's hover class on pointer enter and removes it on leave. A class, not :hover, because the
// badge reads its fill from a variable the button sets, and a :hover change does not restyle the badge.
void TrackPillHover(Button& pill)
{
    pill.RegisterEventHandler(kEventMouseEnter, [&pill](UIEvent&) { pill.AddClass(kPillHoveredClass); });
    pill.RegisterEventHandler(kEventMouseLeave, [&pill](UIEvent&) { pill.RemoveClass(kPillHoveredClass); });
}

// Shows `text` in the pill `label` of `button`; an empty `text` hides the button.
void SetPill(Button& button, Label& label, const std::string& text)
{
    label.SetText(text);
    if (text.empty())
        button.AddClass("hidden");
    else
        button.RemoveClass("hidden");
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
                     UndoRedoService* undo, const PrefabOverridesListContext& overridesContext,
                     SelectAssetHandler selectAsset, std::function<void()> refreshMarks,
                     std::function<void()> requestRebuild)
    : m_World(world), m_Entity(entity), m_Notifications(notifications), m_SelectAsset(std::move(selectAsset)),
      m_Document(overridesContext.Document), m_Reopen(overridesContext.Reopen),
      m_RefreshMarks(std::move(refreshMarks)), m_RequestRebuild(std::move(requestRebuild)),
      m_ShownOverrides(OwnOverridesOf(world, entity))
{
    AddClass("prefab-bar-host");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    std::unique_ptr<UIElement> layout = ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!layout)
        return;
    m_Glyph = layout->FindById("prefab-bar-glyph"_sid);
    m_TitleLead = dynamic_cast<Label*>(layout->FindById("prefab-bar-title-lead"_sid));
    m_TitlePrefab = dynamic_cast<Label*>(layout->FindById("prefab-bar-prefab"_sid));
    m_TitleTail = dynamic_cast<Label*>(layout->FindById("prefab-bar-title-tail"_sid));
    m_Counts = layout->FindById("prefab-bar-counts"_sid);
    m_CountNone = dynamic_cast<Label*>(layout->FindById("prefab-bar-count-none"_sid));
    m_Count = dynamic_cast<Button*>(layout->FindById("prefab-bar-count"_sid));
    m_CountText = dynamic_cast<Label*>(layout->FindById("prefab-bar-count-text"_sid));
    m_InstanceCount = dynamic_cast<Button*>(layout->FindById("prefab-bar-instance-count"_sid));
    m_InstanceCountText = dynamic_cast<Label*>(layout->FindById("prefab-bar-instance-count-text"_sid));
    m_StaleCount = dynamic_cast<Button*>(layout->FindById("prefab-bar-stale-count"_sid));
    m_StaleCountText = dynamic_cast<Label*>(layout->FindById("prefab-bar-stale-count-text"_sid));
    m_SelectAssetButton = dynamic_cast<Button*>(layout->FindById("prefab-bar-select-asset"_sid));
    AddChild(std::move(layout));
    if (m_SelectAssetButton)
        m_SelectAssetButton->SetOnClick([this](UIEvent&) { SelectAsset(); });
    for (Button* pill : {m_Count, m_InstanceCount})
    {
        if (!pill)
            continue;
        pill->SetOnClick([this](UIEvent&) { OpenOverridesList(); });
        TrackPillHover(*pill);
        pill->SetTooltip("Show every override of the instance.");
    }
    if (m_StaleCount)
    {
        m_StaleCount->SetOnClick([this](UIEvent&) { OpenOverridesList(); });
        TrackPillHover(*m_StaleCount);
        m_StaleCount->SetTooltip("Show the overrides on entities that are not in the prefab.");
    }
    auto overridesList = std::make_unique<PrefabOverridesList>(world, undo, notifications, overridesContext);
    m_OverridesList = overridesList.get();
    AddChild(std::move(overridesList));
    if (m_Notifications)
    {
        m_ComponentToken = m_Notifications->SubscribeComponentChanged(
            [this](const EditorChangeNotifications::ComponentChangedEvent& change) { OnComponentChanged(change); });
        m_StructureToken = m_Notifications->SubscribeWorldStructureChanged(
            [this](const EditorChangeNotifications::WorldStructureChangedEvent& change)
            { OnWorldStructureChanged(change); });
    }
    Refresh();
    // The list was open on this instance when a rebuild of the Inspector took the last bar away.
    if (m_Reopen && m_Reopen->World == &m_World && m_Reopen->Root == m_Root)
        OpenOverridesList();
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
    if (!m_Glyph || !m_TitleLead || !m_TitlePrefab || !m_TitleTail || !m_Counts || !m_CountNone || !m_Count ||
        !m_CountText || !m_InstanceCount || !m_InstanceCountText || !m_StaleCount || !m_StaleCountText ||
        !m_World.IsValid(m_Entity))
        return;
    m_Root = FindOutermostInstanceRoot(m_World, m_Entity);
    const TitleParts title = DescribeTitle(m_World, m_Entity);
    SetShownText(*m_TitleLead, title.Lead);
    SetShownText(*m_TitlePrefab, title.Prefab);
    SetShownText(*m_TitleTail, title.Tail);
    if (title.PrefabMissing)
    {
        m_Glyph->RemoveClass(kPrefabGlyphClass);
        m_Glyph->AddClass(kMissingGlyphClass);
        m_TitlePrefab->AddClass(kMissingNameClass);
    }
    else
    {
        m_Glyph->RemoveClass(kMissingGlyphClass);
        m_Glyph->AddClass(kPrefabGlyphClass);
        m_TitlePrefab->RemoveClass(kMissingNameClass);
    }
    // The pills count the rows of the Overrides list: the teal pill its overrides, the amber pill its stale rows.
    SceneDocumentManager* document = m_Document ? m_Document() : nullptr;
    std::span<const Scene::SceneDegradation::Skip> skips;
    if (document && document->GetLastLoadDegraded().has_value())
        skips = document->GetLastLoadDegraded()->Census.skips;
    const PrefabOverrideCounts rows = PrefabOverridesListModel::Count(m_World, m_Entity, skips);
    const std::size_t stale = rows.Stale;
    const CountLines counts = DescribeCounts(m_World, m_Entity, rows.Overrides);
    // An instance whose only rows are stale shows the amber pill alone, never "No overrides".
    const bool showsNone = counts.EntityHasNone && (stale == 0 || !counts.Instance.empty());
    SetShownText(*m_CountNone, showsNone ? counts.Entity : std::string());
    SetPill(*m_Count, *m_CountText, counts.EntityHasNone ? std::string() : counts.Entity);
    SetPill(*m_InstanceCount, *m_InstanceCountText, counts.Instance);
    SetPill(*m_StaleCount, *m_StaleCountText, stale == 0 ? std::string() : std::format("{} not in prefab", stale));
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
    if (m_OverridesList)
        m_OverridesList->Refresh();
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

void PrefabBar::OpenOverridesList()
{
    if (m_OverridesList && m_Counts && m_World.IsValid(m_Entity))
        m_OverridesList->Open(*m_Counts, m_Entity);
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
