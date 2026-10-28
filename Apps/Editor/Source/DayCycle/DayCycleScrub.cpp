#include "DayCycle/DayCycleScrub.h"

#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "DayCycle/DayCycleSunNow.h"
#include "Sky/SkyEnvironmentEdit.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
std::string LightName(const ECS::World& world, ECS::EntityHandle light)
{
    const auto* name = world.GetComponent<Components::Name>(light);
    return name && !name->View().empty() ? std::string(name->View()) : std::string("linked to the sky");
}
} // namespace

DayCycleScrub::DayCycleScrub(ECS::World& world, std::function<Engine::Renderer::SkyEnvironmentSystem*()> sky,
                             UndoRedoService* undo,
                             EditorChangeNotifications* notifications, std::function<bool()> isPlaying)
    : m_World(world), m_Sky(std::move(sky)), m_Undo(undo), m_Notifications(notifications), m_IsPlaying(std::move(isPlaying))
{
    if (!m_Notifications)
        return;
    m_ComponentToken = m_Notifications->SubscribeComponentChanged(
        [this](const EditorChangeNotifications::ComponentChangedEvent& event) { OnComponentChanged(event); });
    m_StructureToken = m_Notifications->SubscribeWorldStructureChanged(
        [this](const EditorChangeNotifications::WorldStructureChangedEvent& event) {
            if (event.world == &m_World)
                CheckScene();
        });
}

DayCycleScrub::~DayCycleScrub()
{
    if (m_Notifications)
    {
        m_Notifications->Unsubscribe(m_ComponentToken);
        m_Notifications->Unsubscribe(m_StructureToken);
    }
    ClearPreview();
}

std::string DayCycleScrub::BlockedNotice() const
{
    if (IsPlaying())
        return "The scrub is off in Play mode.";
    if (!m_Sky || !m_Sky())
        return "The scene's sky renderer is not running.";
    const ECS::EntityHandle skyEntity = Components::SkySunIlluminance::RenderedSky(m_World);
    const auto* sky = skyEntity.IsValid() ? m_World.GetComponent<Components::SkyEnvironment>(skyEntity) : nullptr;
    if (!sky)
        return "The scene has no Sky Environment, so it has no time of day to scrub.";
    if (const ECS::EntityHandle light = DayCycleFollowedLight(m_World, *sky); light.IsValid())
        return "The sun follows the light " + LightName(m_World, light) +
               "; rotate the light, or turn on Sky drives sun light to scrub the time of day.";
    if (!sky->AutoSunMoon)
        return "The sun is set by hand; turn on Auto Sun/Moon to scrub the time of day.";
    return {};
}

void DayCycleScrub::SetHeld(bool held)
{
    if (held == m_Held)
        return;
    m_Held = held;
    if (held)
        return;
    const std::optional<float> hours = PreviewHour();
    if (!hours)
        return;
    const ECS::EntityHandle skyEntity = Components::SkySunIlluminance::RenderedSky(m_World);
    const auto* sky = skyEntity.IsValid() ? m_World.GetComponent<Components::SkyEnvironment>(skyEntity) : nullptr;
    if (!sky || !sky->AnimateTimeOfDay)
        return;
    // A running clock goes on from where the playhead was let go: the hour becomes the scene's.
    m_Releasing = true;
    CommitSkyEnvironmentEdit(&m_World, skyEntity, {}, m_Notifications, m_Undo, "Scrub Time of Day",
                             [hour = *hours](Components::SkyEnvironment& edited) { edited.TimeOfDayHours = hour; });
    m_Releasing = false;
    ClearPreview();
}

void DayCycleScrub::ScrubTo(float hours)
{
    if (!BlockedNotice().empty())
        return;
    auto* sky = m_Sky ? m_Sky() : nullptr;
    if (!sky)
        return;
    sky->SetPreviewHour(m_World.GetWorldId(), hours);
    m_PreviewSky = Components::SkySunIlluminance::RenderedSky(m_World);
    m_PreviewResetGeneration = m_World.GetLifecycleResetGeneration();
}

std::optional<float> DayCycleScrub::PreviewHour() const
{
    const auto* sky = m_Sky ? m_Sky() : nullptr;
    return sky ? sky->GetPreviewHour(m_World.GetWorldId()) : std::nullopt;
}

void DayCycleScrub::CheckScene()
{
    if (!PreviewHour())
        return;
    if (IsPlaying() || m_World.GetLifecycleResetGeneration() != m_PreviewResetGeneration ||
        Components::SkySunIlluminance::RenderedSky(m_World) != m_PreviewSky)
        ClearPreview();
}

bool DayCycleScrub::IsPlaying() const
{
    return m_IsPlaying && m_IsPlaying();
}

void DayCycleScrub::ClearPreview()
{
    if (auto* sky = m_Sky ? m_Sky() : nullptr)
        sky->ClearPreviewHour(m_World.GetWorldId());
}

void DayCycleScrub::OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event)
{
    // An edit to the sky (its Time of Day field among them) ends the preview so the edit shows; a
    // drag's live previews of other fields do too, as they show the scene's own hour.
    if (m_Releasing || event.world != &m_World ||
        event.componentType != ECS::GetComponentTypeId<Components::SkyEnvironment>() ||
        event.kind == EditorChangeNotifications::ChangeKind::InspectorRebuild)
        return;
    ClearPreview();
}

} // namespace GameEngine::Editor
