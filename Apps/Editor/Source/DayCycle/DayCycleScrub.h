#pragma once

#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
}
namespace Engine::Renderer
{
class SkyEnvironmentSystem;
}
} // namespace GameEngine

namespace GameEngine::Editor
{
class UndoRedoService;

/// The Day Cycle window's scrub of the open scene's time of day. A drag sets the world's preview
/// hour on the sky system, which every view of the world renders in place of the Sky Environment's
/// own hour; the component is never written, so the scene's unsaved mark, its save and undo do not
/// see a scrub. The preview ends when this object goes (the window closes), when the sky is edited
/// (an edit to its Time of Day field then shows), when the scene it was set for is gone (a scene
/// opened or closed, the sky entity removed) and when play starts; the scrub is off while the editor
/// plays. Under AnimateTimeOfDay the clock holds while the playhead is held, and the release writes
/// the scrubbed hour to the component as one undoable edit, so the clock runs on from there. Main
/// thread only.
class DayCycleScrub
{
  public:
    /// `isPlaying` says whether the editor plays; null means never.
    DayCycleScrub(ECS::World& world, Engine::Renderer::SkyEnvironmentSystem& sky, UndoRedoService* undo,
                  EditorChangeNotifications* notifications, std::function<bool()> isPlaying);
    ~DayCycleScrub();
    DayCycleScrub(const DayCycleScrub&) = delete;
    DayCycleScrub& operator=(const DayCycleScrub&) = delete;

    /// Why the scrub is off, as the notice the window shows; empty while the scrub works. The editor
    /// plays, the scene has no sky, the sky follows a linked light, or its sun is set by hand.
    std::string BlockedNotice() const;

    /// The playhead is pressed (true) or released (false).
    void SetHeld(bool held);
    /// The playhead was dragged to `hours` of local solar time. Ignored while the scrub is blocked.
    void ScrubTo(float hours);
    /// The hour the world previews, if any.
    std::optional<float> PreviewHour() const;
    /// Ends the preview when play has started or the scene it was set for is gone: the world was
    /// reset (a scene opened or closed) or its rendered sky is another entity. The window calls this
    /// on its refresh; a world structure change calls it too.
    void CheckScene();

  private:
    void ClearPreview();
    bool IsPlaying() const;
    void OnComponentChanged(const EditorChangeNotifications::ComponentChangedEvent& event);

    ECS::World& m_World;
    Engine::Renderer::SkyEnvironmentSystem& m_Sky;
    UndoRedoService* m_Undo = nullptr;
    EditorChangeNotifications* m_Notifications = nullptr;
    std::function<bool()> m_IsPlaying;
    EditorChangeNotifications::SubscriptionToken m_ComponentToken{};
    EditorChangeNotifications::SubscriptionToken m_StructureToken{};
    // The scene the preview was set for: the rendered sky and the world's reset generation.
    ECS::EntityHandle m_PreviewSky{};
    std::uint64_t m_PreviewResetGeneration = 0;
    bool m_Held = false;
    // Set while the release's own write to the component goes out, which must not end the preview
    // it is ending itself.
    bool m_Releasing = false;
};

} // namespace GameEngine::Editor
