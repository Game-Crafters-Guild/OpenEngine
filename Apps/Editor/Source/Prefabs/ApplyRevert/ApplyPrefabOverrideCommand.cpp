#include "Prefabs/ApplyRevert/ApplyPrefabOverrideCommand.h"

#include "EditorChangeNotifications.h"

#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Scene/SceneSchemaRegistry.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace GameEngine::Editor
{

ApplyPrefabOverrideCommand::ApplyPrefabOverrideCommand(ConstructionKey, ECS::World& world,
                                                       EditorChangeNotifications* notifications, std::string name)
    : m_Name(std::move(name))
    , m_World(world)
    , m_Notifications(notifications)
{
}

std::unique_ptr<ApplyPrefabOverrideCommand> ApplyPrefabOverrideCommand::Create(
    ECS::World& world, EditorChangeNotifications* notifications, std::span<const Scene::PrefabOverrideTarget> targets,
    std::string name, std::string& outError)
{
    auto command = std::make_unique<ApplyPrefabOverrideCommand>(ConstructionKey{}, world, notifications, std::move(name));
    // The undo writes the old text, which gives each applier the old prefab value: these components put its own back.
    for (const Scene::PrefabOverrideTarget& target : targets)
    {
        auto applier = std::find_if(command->m_Appliers.begin(), command->m_Appliers.end(),
                                    [&target](const Applier& a) { return a.Entity == target.Entity; });
        if (applier == command->m_Appliers.end())
        {
            command->m_Appliers.push_back({target.Entity, {}, CaptureEntityComponents(world, target.Entity)});
            applier = std::prev(command->m_Appliers.end());
        }
        if (const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(target.Override.Component))
            applier->Components.push_back(Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema));
    }
    Scene::ApplyPrefabOverrides(world, targets, command->m_Edits, outError);
    if (command->m_Edits.empty())
        return nullptr;
    return command;
}

void ApplyPrefabOverrideCommand::Undo()
{
    for (auto edit = m_Edits.rbegin(); edit != m_Edits.rend(); ++edit)
        WriteText(*edit, edit->Before);
    for (const Applier& applier : m_Appliers)
        if (m_World.IsValid(applier.Entity))
            RestoreEntityComponents(m_World, applier.Entity, applier.Before, m_Notifications);
    NotifyWorldStructure(m_Notifications, &m_World);
}

void ApplyPrefabOverrideCommand::Redo()
{
    for (const Scene::PrefabFileEdit& edit : m_Edits)
        WriteText(edit, edit.After);
    NotifyWorldStructure(m_Notifications, &m_World);
    if (!m_Notifications)
        return;
    for (const Applier& applier : m_Appliers)
        if (m_World.IsValid(applier.Entity))
            for (const ECS::ComponentTypeId component : applier.Components)
                m_Notifications->NotifyComponentChanged(
                    {&m_World, applier.Entity, component, EditorChangeNotifications::ChangeKind::UndoRedo});
}

void ApplyPrefabOverrideCommand::WriteText(const Scene::PrefabFileEdit& edit, const std::string& text)
{
    std::string error;
    if (!Scene::WritePrefabFileText(edit.File, text, error))
        Logger::Log::Error("Prefabs: {} could not write the prefab: {}", m_Name, error);
}

} // namespace GameEngine::Editor
