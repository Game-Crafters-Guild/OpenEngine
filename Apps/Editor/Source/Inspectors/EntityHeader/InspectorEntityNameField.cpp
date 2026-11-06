#include "Inspectors/EntityHeader/InspectorEntityNameField.h"

#include "Editor/Entities/EntityDisplayName.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "UndoRedo/History/HistoryReplayScope.h"
#include "UndoRedo/History/HistoryValueFormatter.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <cstring>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr std::size_t kMaxEntityNameBytes = sizeof(Components::Name::value) - 1;
constexpr const char* kRenameEntityLabel = "Rename Entity";

bool RestoreEntityName(ECS::World& world, ECS::EntityHandle entity,
                       const Editor::HistorySnapshotTarget::Snapshot& bytes)
{
    if (bytes.empty())
    {
        world.RemoveComponentImmediate<Components::Name>(entity);
        return true;
    }
    return world.ApplyComponentBytesImmediate(entity, ECS::GetComponentTypeId<Components::Name>(), bytes);
}

Editor::HistorySnapshotTarget MakeEntityNameTarget(const InspectorContext& context)
{
    ECS::World* world = context.World;
    const ECS::EntityHandle entity = context.Entity;
    const auto worldId = world->GetWorldId();
    const auto epoch = world->GetEntityHandleEpoch();
    const auto typeId = ECS::GetComponentTypeId<Components::Name>();
    Editor::HistorySnapshotTarget target;
    target.debugLabel = kRenameEntityLabel;
    target.Identity = {worldId, epoch, typeId, entity.id};
    target.Describe = [typeId](const auto& before, const auto& after)
    {
        return Editor::HistoryValueFormatter::DescribeComponent(typeId, before, after);
    };
    target.Validate = [world, entity, worldId, epoch]
    {
        const auto liveEpoch = ECS::World::FindLiveEntityHandleEpoch(worldId);
        return liveEpoch && *liveEpoch == epoch && world->IsValid(entity);
    };
    target.Capture = [world, entity, typeId, validate = target.Validate](auto& bytes)
    {
        if (!validate())
            return false;
        bytes.clear();
        return !world->HasComponent<Components::Name>(entity) || world->CaptureComponentBytes(entity, typeId, bytes);
    };
    target.Apply = [world, entity, validate = target.Validate](const auto& bytes)
    {
        return validate() && RestoreEntityName(*world, entity, bytes);
    };
    target.Notify = [world, entity, typeId, notifications = context.ChangeNotifications,
                     validate = target.Validate](Editor::EditChangeKind kind)
    {
        if (!notifications || !validate())
            return;
        const auto changeKind = kind == Editor::EditChangeKind::UndoRedo && !Editor::HistoryReplayScope::IsActive()
            ? Editor::EditChangeKind::Commit : kind;
        notifications->NotifyComponentChanged({world, entity, typeId, changeKind});
    };
    return target;
}
}

InspectorEntityNameField::InspectorEntityNameField(const InspectorContext& context,
                                                 std::function<void()> componentSetChanged)
    : m_World(context.World), m_Entity(context.Entity), m_Undo(context.Undo),
      m_Target(MakeEntityNameTarget(context)), m_InitialText(Editor::EntityDisplayName(*m_World, m_Entity)),
      m_ComponentSetChanged(std::move(componentSetChanged))
{
    m_Target.Capture(m_InitialName);
    if (context.Entities.size() > 1)
        m_SelectionSuffix = " (+" + std::to_string(context.Entities.size() - 1) + " more)";
    AddClass("inspector-header");
    SetValue(m_InitialText + m_SelectionSuffix);
    SetOnValueChanging([this](const std::string& text) { PreviewName(text); });
    SetOnCommit([this] { CommitName(); });
}

void InspectorEntityNameField::PreviewName(const std::string& text)
{
    std::string name = text;
    if (!m_SelectionSuffix.empty() && name.ends_with(m_SelectionSuffix))
        name.resize(name.size() - m_SelectionSuffix.size());
    if (name.size() > kMaxEntityNameBytes)
        name.resize(kMaxEntityNameBytes);
    Editor::HistorySnapshotTarget::Snapshot desired;
    if (name == m_InitialText)
        desired = m_InitialName;
    else
    {
        Components::Name value{};
        std::memcpy(value.value, name.data(), name.size());
        desired.resize(sizeof(value));
        std::memcpy(desired.data(), &value, sizeof(value));
    }
    Editor::HistorySnapshotTarget::Snapshot current;
    if (!m_Target.Capture(current) || current == desired)
        return;
    if (m_Undo)
    {
        auto edit = m_Undo->BeginInteractiveEdit(kRenameEntityLabel, m_Target);
        if (!edit)
            return;
        edit.Preview([this, &desired] { ApplyName(desired); });
        edit.Commit();
    }
    else
    {
        ApplyName(desired);
        m_Target.Notify(Editor::EditChangeKind::Commit);
    }
}

void InspectorEntityNameField::ApplyName(const Editor::HistorySnapshotTarget::Snapshot& bytes)
{
    const bool hadName = m_World->HasComponent<Components::Name>(m_Entity);
    if (m_Target.Apply(bytes) && hadName != m_World->HasComponent<Components::Name>(m_Entity) && m_ComponentSetChanged)
        m_ComponentSetChanged();
}

void InspectorEntityNameField::CommitName()
{
    PreviewName(GetTextInput()->GetValue());
    if (m_Undo && !m_Undo->CommitPendingEdit())
        return;
    if (m_Target.Capture(m_InitialName))
    {
        const auto* name = m_World->GetComponent<Components::Name>(m_Entity);
        m_InitialText = name ? std::string(name->View()) : Editor::EntityDisplayName(*m_World, m_Entity);
    }
}
}
