#include "Prefabs/PrefabOverrideRecordsCommand.h"

#include "EditorChangeNotifications.h"

#include "ECS/Entity.h"
#include "Scene/SceneSchemaRegistry.h"

#include <utility>

namespace GameEngine::Editor
{

PrefabOverrideRecordsCommand::PrefabOverrideRecordsCommand(ECS::World& world, EditorChangeNotifications* notifications,
                                                           std::span<const Scene::PrefabOverrideTarget> added,
                                                           std::span<const Scene::PrefabOverrideTarget> dropped,
                                                           std::string name)
    : m_Name(std::move(name))
    , m_World(world)
    , m_Notifications(notifications)
    , m_Added(added.begin(), added.end())
    , m_Dropped(dropped.begin(), dropped.end())
{
}

void PrefabOverrideRecordsCommand::Undo()
{
    Scene::ForgetPrefabOverrides(m_World, m_Added);
    Scene::RecordPrefabOverrides(m_World, m_Dropped);
    NotifyTargets(m_Added);
    NotifyTargets(m_Dropped);
}

void PrefabOverrideRecordsCommand::Redo()
{
    Scene::RecordPrefabOverrides(m_World, m_Added);
    Scene::ForgetPrefabOverrides(m_World, m_Dropped);
    NotifyTargets(m_Added);
    NotifyTargets(m_Dropped);
}

void PrefabOverrideRecordsCommand::NotifyTargets(std::span<const Scene::PrefabOverrideTarget> targets) const
{
    if (!m_Notifications)
        return;
    for (const Scene::PrefabOverrideTarget& target : targets)
    {
        const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(target.Override.Component);
        if (schema && m_World.IsValid(target.Entity))
            m_Notifications->NotifyComponentChanged({&m_World, target.Entity,
                                                     Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema),
                                                     EditorChangeNotifications::ChangeKind::UndoRedo});
    }
}

} // namespace GameEngine::Editor
