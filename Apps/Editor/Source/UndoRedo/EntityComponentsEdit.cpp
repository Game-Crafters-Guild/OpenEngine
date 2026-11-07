#include "UndoRedo/EntityComponentsEdit.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Component/ComponentRegistry.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/History/HistoryValueFormatter.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using ComponentBytes = std::vector<std::uint8_t>;

// One component the edit touched: its bytes before and after, absent on the side where the
// entity did not carry it.
struct ComponentChange
{
    ECS::ComponentTypeId TypeId = 0;
    std::optional<ComponentBytes> Before;
    std::optional<ComponentBytes> After;
};

// The components whose presence or bytes differ between two captures of one entity.
std::vector<ComponentChange> DiffEntityComponents(const EntityComponentBytes& before, const EntityComponentBytes& after)
{
    std::vector<ComponentChange> changes;
    auto b = before.begin();
    auto a = after.begin();
    while (b != before.end() || a != after.end())
    {
        if (a == after.end() || (b != before.end() && b->first < a->first))
        {
            changes.push_back({b->first, b->second, std::nullopt});
            ++b;
        }
        else if (b == before.end() || a->first < b->first)
        {
            changes.push_back({a->first, std::nullopt, a->second});
            ++a;
        }
        else
        {
            if (b->second != a->second)
                changes.push_back({a->first, b->second, a->second});
            ++b;
            ++a;
        }
    }
    return changes;
}

bool WriteComponentChange(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                           const std::optional<ComponentBytes>& bytes)
{
    if (bytes)
        return world.ApplyComponentBytesImmediate(entity, typeId, *bytes);
    return !world.HasComponent(entity, typeId) || world.RemoveComponentByTypeIdImmediate(entity, typeId);
}

void NotifyComponentChanges(ECS::World& world, ECS::EntityHandle entity,
                            const std::vector<ComponentChange>& changes, EditorChangeNotifications* notifications)
{
    if (notifications)
        for (const ComponentChange& change : changes)
            notifications->NotifyComponentChanged({&world, entity, change.TypeId, EditChangeKind::UndoRedo});
}

// Validate every payload before writing and restore prior values if a write fails.
bool ApplyComponentChanges(ECS::World& world, ECS::EntityHandle entity, const std::vector<ComponentChange>& changes,
                           std::optional<ComponentBytes> ComponentChange::*side, EditorChangeNotifications* notifications)
{
    if (!world.IsValid(entity))
        return false;
    std::vector<std::optional<ComponentBytes>> before;
    before.reserve(changes.size());
    for (const ComponentChange& change : changes)
    {
        const auto* handler = ECS::ComponentRegistry::GetHandler(change.TypeId);
        const auto& bytes = change.*side;
        if (!handler || (bytes && bytes->size() != handler->GetComponentSize()))
            return false;
        ComponentBytes current;
        if (world.CaptureComponentBytes(entity, change.TypeId, current))
            before.emplace_back(std::move(current));
        else if (world.HasComponent(entity, change.TypeId))
            return false;
        else
            before.emplace_back(std::nullopt);
    }
    for (std::size_t index = 0; index < changes.size(); ++index)
    {
        const auto& change = changes[index];
        if (WriteComponentChange(world, entity, change.TypeId, change.*side))
            continue;
        while (index > 0)
        {
            --index;
            WriteComponentChange(world, entity, changes[index].TypeId, before[index]);
        }
        return false;
    }
    NotifyComponentChanges(world, entity, changes, notifications);
    return true;
}

class EntityComponentsEditCommand final : public IEditorCommand
{
public:
    EntityComponentsEditCommand(std::string label, ECS::World& world, ECS::EntityHandle entity,
                                EditorChangeNotifications* notifications, std::vector<ComponentChange> changes)
        : m_Label(std::move(label)),
          m_World(world),
          m_Entity(entity),
          m_Notifications(notifications),
          m_Changes(std::move(changes))
    {
        for (const auto& change : m_Changes)
        {
            const ComponentBytes empty;
            const std::string description = HistoryValueFormatter::DescribeComponent(change.TypeId,
                change.Before ? *change.Before : empty, change.After ? *change.After : empty);
            if (description.empty())
                continue;
            if (!m_Description.empty())
                m_Description += '\n';
            m_Description += description;
        }
    }

    const char* GetName() const override { return m_Label.c_str(); }
    std::string GetDescription() const override { return m_Description; }
    const char* GetTypeName() const override { return "EntityComponentsEditCommand"; }
    void Do() override { if (TryRedo()) NotifyReplay(); }
    void Undo() override { if (TryUndo()) NotifyReplay(); }
    bool TryUndo() override { return Apply(&ComponentChange::Before); }
    bool TryRedo() override { return Apply(&ComponentChange::After); }
    void NotifyReplay() override { NotifyComponentChanges(m_World, m_Entity, m_Changes, m_Notifications); }

private:
    bool Apply(std::optional<ComponentBytes> ComponentChange::*side)
    {
        return ApplyComponentChanges(m_World, m_Entity, m_Changes, side, nullptr);
    }

    std::string m_Label;
    std::string m_Description;
    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    EditorChangeNotifications* m_Notifications;
    std::vector<ComponentChange> m_Changes;
};

} // namespace

EntityComponentBytes CaptureEntityComponents(const ECS::World& world, ECS::EntityHandle entity)
{
    EntityComponentBytes components;
    const ECS::Archetype* archetype = world.GetEntityArchetype(entity);
    if (!archetype)
        return components;
    for (const ECS::ComponentTypeId typeId : archetype->GetSignature().GetComponents())
    {
        ComponentBytes bytes;
        if (world.CaptureComponentBytes(entity, typeId, bytes))
            components.emplace_back(typeId, std::move(bytes));
    }
    std::sort(components.begin(), components.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return components;
}

void RestoreEntityComponents(ECS::World& world, ECS::EntityHandle entity, const EntityComponentBytes& captured,
                             EditorChangeNotifications* notifications)
{
    const std::vector<ComponentChange> changes = DiffEntityComponents(CaptureEntityComponents(world, entity), captured);
    ApplyComponentChanges(world, entity, changes, &ComponentChange::After, notifications);
}

bool CommitEntityComponentsEdit(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                                EditorChangeNotifications* notifications, const std::string& label,
                                const std::function<bool()>& edit)
{
    if (!world.IsValid(entity))
        return false;
    const EntityComponentBytes before = CaptureEntityComponents(world, entity);
    bool applied = false;
    try
    {
        applied = edit();
    }
    catch (...)
    {
        RestoreEntityComponents(world, entity, before, notifications);
        throw;
    }
    if (!applied)
    {
        RestoreEntityComponents(world, entity, before, notifications);
        return false;
    }
    std::vector<ComponentChange> changes = DiffEntityComponents(before, CaptureEntityComponents(world, entity));
    if (undo && !changes.empty())
    {
        undo->CommitAlreadyApplied(
            std::make_unique<EntityComponentsEditCommand>(label, world, entity, notifications, std::move(changes)));
    }
    return applied;
}

} // namespace GameEngine::Editor
