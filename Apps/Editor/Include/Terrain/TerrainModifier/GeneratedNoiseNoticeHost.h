#pragma once

#include "EditorChangeNotifications.h"
#include "UI/UIElement.h"

#include <functional>

namespace GameEngine
{
namespace ECS
{
class World;
}

namespace Editor
{

/// An Inspector row that holds the notices that depend on the scene's Generated noise. The host builds
/// its notices again when a terrain, a modifier volume, a Noise effect or a name changes, or when an
/// entity is created, deleted, enabled or disabled, so the notices are correct without a reselection.
class GeneratedNoiseNoticeHost final : public UIElement
{
public:
    /// `build` adds the notices to the host. With no `notifications`, the host builds them one time.
    GeneratedNoiseNoticeHost(ECS::World* world, EditorChangeNotifications* notifications,
                             std::function<void(UIElement&)> build);
    ~GeneratedNoiseNoticeHost() override;

private:
    void Rebuild();
    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event);

    ECS::World* m_World = nullptr;
    EditorChangeNotifications* m_Notifications = nullptr;
    std::function<void(UIElement&)> m_Build;
    EditorChangeNotifications::SubscriptionToken m_ComponentSubscription{};
    EditorChangeNotifications::SubscriptionToken m_StructureSubscription{};
};

} // namespace Editor
} // namespace GameEngine
