#pragma once

#include "ECS/Entity.h"

#include <functional>
#include <vector>

namespace GameEngine
{
class AssetManager;
namespace ECS
{
class World;
}
namespace Engine::Renderer
{
class RenderServices;
class SkyEnvironmentSystem;
} // namespace Engine::Renderer
} // namespace GameEngine

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

/// What the Day Cycle window and the profile's inspector read and write through. Each getter may
/// return null (no scene open, no renderer yet); the surfaces then show what they can.
struct DayCycleEditorServices
{
    AssetManager* Assets = nullptr;
    UndoRedoService* Undo = nullptr;
    EditorChangeNotifications* Notifications = nullptr;
    std::function<ECS::World*()> World;
    std::function<Engine::Renderer::RenderServices*()> Render;
    std::function<Engine::Renderer::SkyEnvironmentSystem*()> Sky;
    /// The entities selected in the editor.
    std::function<std::vector<ECS::EntityHandle>()> Selection;
    /// Whether the editor plays (or is paused in play). Null: never.
    std::function<bool()> IsPlaying;
};

/// The editor's own: the engine's asset manager, primary world, renderer and the sky system on the
/// rendering loop's schedule, with the given undo history, notifications, selection and play state.
DayCycleEditorServices EditorDayCycleServices(UndoRedoService* undo, EditorChangeNotifications* notifications,
                                              std::function<std::vector<ECS::EntityHandle>()> selection,
                                              std::function<bool()> isPlaying);

} // namespace GameEngine::Editor
