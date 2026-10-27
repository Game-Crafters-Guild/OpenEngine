#include "Prefabs/UnpackPrefabCommand.h"

#include "EditorChangeNotifications.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

#include <format>
#include <utility>

namespace GameEngine::Editor
{

UnpackPrefabCommand::UnpackPrefabCommand(ConstructionKey, ECS::World& world, EditorChangeNotifications* notifications,
                                         ECS::EntityHandle root, Scene::PrefabUnpackDepth depth)
    : m_World(world)
    , m_Notifications(notifications)
    , m_Root(root)
    , m_Depth(depth)
{
    const auto* name = world.GetComponent<Components::Name>(root);
    const std::string verb = depth == Scene::PrefabUnpackDepth::Completely ? "Unpack Completely" : "Unpack";
    m_Name = name && !name->View().empty() ? std::format("{} {}", verb, name->View()) : verb;
}

std::unique_ptr<UnpackPrefabCommand> UnpackPrefabCommand::Create(ECS::World& world,
                                                                 EditorChangeNotifications* notifications,
                                                                 ECS::EntityHandle root,
                                                                 Scene::PrefabUnpackDepth depth,
                                                                 std::string& outError)
{
    auto command = std::make_unique<UnpackPrefabCommand>(ConstructionKey{}, world, notifications, root, depth);
    command->m_Before = Scene::UnpackPrefabInstance(world, root, depth, outError);
    if (command->m_Before.empty())
        return nullptr;
    NotifyWorldStructure(notifications, &world);
    return command;
}

void UnpackPrefabCommand::Undo()
{
    Scene::RestorePrefabIdentities(m_World, m_Before);
    NotifyWorldStructure(m_Notifications, &m_World);
}

void UnpackPrefabCommand::Redo()
{
    std::string error;
    m_Before = Scene::UnpackPrefabInstance(m_World, m_Root, m_Depth, error);
    if (m_Before.empty())
        Logger::Log::Error("Prefabs: the redo of {} failed: {}", m_Name, error);
    NotifyWorldStructure(m_Notifications, &m_World);
}

} // namespace GameEngine::Editor
