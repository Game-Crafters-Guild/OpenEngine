#include "Terrain/TerrainModifier/GeneratedNoiseNoticeHost.h"

#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECSTemplates.h"

#include <utility>

namespace GameEngine::Editor
{

GeneratedNoiseNoticeHost::GeneratedNoiseNoticeHost(ECS::World* world, EditorChangeNotifications* notifications,
                                                   std::function<void(UIElement&)> build)
    : m_World(world)
    , m_Notifications(notifications)
    , m_Build(std::move(build))
{
    Rebuild();
    if (!m_Notifications)
        return;
    m_ComponentSubscription = m_Notifications->SubscribeComponentChanged(
        [this](const EditorChangeNotifications::ComponentChangedEvent& event) { OnComponentChanged(event); });
    m_StructureSubscription = m_Notifications->SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent& event) {
            if (event.world == m_World)
                Rebuild();
        });
}

GeneratedNoiseNoticeHost::~GeneratedNoiseNoticeHost()
{
    if (!m_Notifications)
        return;
    m_Notifications->Unsubscribe(m_ComponentSubscription);
    m_Notifications->Unsubscribe(m_StructureSubscription);
}

void GeneratedNoiseNoticeHost::Rebuild()
{
    RemoveAllChildren();
    if (m_Build)
        m_Build(*this);
}

void GeneratedNoiseNoticeHost::OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event)
{
    if (event.world != m_World || event.kind == EditorChangeNotifications::ChangeKind::Preview)
        return;
    const ECS::ComponentTypeId type = event.componentType;
    if (type == ECS::GetComponentTypeId<Components::Terrain>() ||
        type == ECS::GetComponentTypeId<Components::TerrainModifierVolume>() ||
        type == ECS::GetComponentTypeId<Components::TerrainNoiseEffect>() ||
        type == ECS::GetComponentTypeId<Components::Name>())
        Rebuild();
}

} // namespace GameEngine::Editor
