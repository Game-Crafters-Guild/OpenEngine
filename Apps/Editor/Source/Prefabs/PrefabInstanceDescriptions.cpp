#include "Prefabs/PrefabInstanceDescriptions.h"

#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabOverrideRecordsCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/Component/ComponentFieldRegistry.h"
#include "ECS/Entity.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

// The name of the records command. The step it joins keeps the name of the edit.
static constexpr const char* kKeepOverridesLabel = "Keep prefab overrides";

static uint64 NewestUndoStep(const UndoRedoService* undo)
{
    const std::size_t count = undo ? undo->GetUndoCount() : 0;
    return count > 0 ? undo->GetUndoEntryIdAt(count - 1) : 0;
}

const Scene::PrefabInstanceEntity* DescribedPrefabInstance::Find(ECS::EntityHandle entity) const
{
    const uint32* index = m_IndexOf.Find(entity.id);
    if (!index || Entities[*index].Entity != entity)
        return nullptr;
    return &Entities[*index];
}

PrefabInstanceDescriptions& PrefabInstanceDescriptions::Get()
{
    static PrefabInstanceDescriptions s_Descriptions;
    return s_Descriptions;
}

void PrefabInstanceDescriptions::Track(EditorChangeNotifications& notifications, UndoRedoService* undo,
                                       std::function<bool()> isInPlayMode)
{
    m_Undo = undo;
    m_IsInPlayMode = std::move(isInPlayMode);
    const uint64 channel = notifications.GetChannelId();
    if (std::find(m_TrackedChannels.begin(), m_TrackedChannels.end(), channel) != m_TrackedChannels.end())
        return;
    m_TrackedChannels.push_back(channel);
    notifications.SubscribeComponentChanged(
        [this, &notifications](const EditorChangeNotifications::ComponentChangedEvent& change)
        { OnComponentChanged(notifications, change); });
    notifications.SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent& change) { OnWorldStructureChanged(change); });
}

const DescribedPrefabInstance* PrefabInstanceDescriptions::FindInstance(ECS::World& world, ECS::EntityHandle entity)
{
    ResetForWorld(world);
    const ECS::EntityHandle root = FindOutermostInstanceRoot(world, entity);
    if (!root.IsValid())
        return nullptr;
    Entry* entry = m_Instances.Find(root.id);
    if (!entry || entry->Root != root)
    {
        entry = &m_Instances.InsertOrAssign(root.id, Entry{root, nullptr});
        if (Scene::DescribePrefabInstance(world, root, m_Scratch))
        {
            auto description = std::make_unique<DescribedPrefabInstance>();
            description->Root = root;
            description->Entities = std::move(m_Scratch);
            description->m_IndexOf.Reserve(description->Entities.size());
            for (uint32 index = 0; index < description->Entities.size(); ++index)
                description->m_IndexOf.InsertOrAssign(description->Entities[index].Entity.id, index);
            entry->Description = std::move(description);
        }
        m_Scratch.clear();
    }
    return entry->Description.get();
}

const Scene::PrefabInstanceEntity* PrefabInstanceDescriptions::FindEntity(ECS::World& world, ECS::EntityHandle entity)
{
    const DescribedPrefabInstance* instance = FindInstance(world, entity);
    return instance ? instance->Find(entity) : nullptr;
}

void PrefabInstanceDescriptions::Forget(ECS::World& world, ECS::EntityHandle entity)
{
    ResetForWorld(world);
    const ECS::EntityHandle root = FindOutermostInstanceRoot(world, entity);
    if (root.IsValid())
        m_Instances.Erase(root.id);
}

void PrefabInstanceDescriptions::ResetForWorld(const ECS::World& world)
{
    if (m_WorldId == world.GetWorldId() && m_Epoch == world.GetEntityHandleEpoch())
        return;
    m_WorldId = world.GetWorldId();
    m_Epoch = world.GetEntityHandleEpoch();
    m_Instances.Clear();
}

void PrefabInstanceDescriptions::OnComponentChanged(EditorChangeNotifications& notifications,
                                                    const EditorChangeNotifications::ComponentChangedEvent& change)
{
    // A drag reports each step; the description changes with the commit.
    if (change.kind == EditorChangeNotifications::ChangeKind::Preview || !change.world ||
        change.world->GetWorldId() != m_WorldId)
        return;
    const ECS::EntityHandle root = FindOutermostInstanceRoot(*change.world, change.entity);
    if (!root.IsValid())
        return;
    // Stop restores the edit world, so a record written in Play would outlive the value it marks.
    const bool inPlay = m_IsInPlayMode && m_IsInPlayMode();
    if (change.kind == EditorChangeNotifications::ChangeKind::Commit && !inPlay)
        RecordOverridesBeforeEdit(notifications, change, root);
    m_Instances.Erase(root.id);
}

void PrefabInstanceDescriptions::RecordOverridesBeforeEdit(EditorChangeNotifications& notifications,
                                                           const EditorChangeNotifications::ComponentChangedEvent& change,
                                                           ECS::EntityHandle root)
{
    // The records join an open compound, or the newest undo entry when the interactive edit that announces this
    // commit is in it. An edit in no entry gives no record. A commit outside the undo history keeps its records
    // with no step.
    const bool inCompound = m_Undo && m_Undo->HasOpenCompound();
    const std::optional<uint64> editEntry = m_Undo ? m_Undo->GetAnnouncedEditEntryId() : std::nullopt;
    if (editEntry && !inCompound &&
        (*editEntry == 0 || *editEntry != NewestUndoStep(m_Undo) || !m_Undo->CanJoinTop()))
        return;
    if (m_Epoch != change.world->GetEntityHandleEpoch())
        return;
    const Entry* entry = m_Instances.Find(root.id);
    if (!entry || entry->Root != root || !entry->Description)
        return;
    const Scene::PrefabInstanceEntity* described = entry->Description->Find(change.entity);
    const std::string_view token = Scene::SceneSchemaRegistry::SchemaTokenForComponentType(change.componentType);
    if (!described || token.empty())
        return;
    std::vector<Scene::PrefabOverrideTarget> targets;
    for (const Scene::PrefabOverride& prefabOverride : described->Overrides)
        if (prefabOverride.Kind == Scene::PrefabOverrideKind::Field && prefabOverride.Component == token)
            targets.push_back({change.entity, prefabOverride});
    const std::vector<Scene::PrefabOverrideTarget> added = Scene::RecordPrefabOverrides(*change.world, targets);
    if (added.empty() || !m_Undo)
        return;
    auto command = std::make_unique<PrefabOverrideRecordsCommand>(
        *change.world, &notifications, added, std::span<const Scene::PrefabOverrideTarget>(), kKeepOverridesLabel);
    if (inCompound)
        m_Undo->CommitAlreadyApplied(std::move(command));
    else if (editEntry)
        m_Undo->CommitJoiningTop(std::move(command));
}

void PrefabInstanceDescriptions::OnWorldStructureChanged(
    const EditorChangeNotifications::WorldStructureChangedEvent& change)
{
    // An entity moved into, out of or between instances.
    if (change.world && change.world->GetWorldId() == m_WorldId)
        m_Instances.Clear();
}

std::string_view InspectorFieldForSceneKey(ECS::ComponentTypeId typeId, std::string_view key)
{
    std::string_view arrayField;
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        if (EqualsIgnoreCase(key, field.Name))
            return field.Name;
        const bool indexFollows = key.size() > field.Name.size() && StartsWithIgnoreCase(key, field.Name) &&
                                  key[field.Name.size()] >= '0' && key[field.Name.size()] <= '9';
        if (indexFollows && field.Name.size() > arrayField.size())
            arrayField = field.Name;
    }
    return arrayField.empty() ? key : arrayField;
}

} // namespace GameEngine::Editor
