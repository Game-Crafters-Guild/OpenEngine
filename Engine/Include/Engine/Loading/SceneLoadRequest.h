#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace GameEngine
{
class SceneLoadInstantiated;

/// What a load does with the entities that the primary world holds.
enum class SceneLoadMode : uint8_t
{
    /// Clears the world, then instantiates the scene: the scene replaces the one that the game shows.
    Single,
    /// Adds the scene's entities to the running world and keeps its entities and its clock.
    Additive
};

/// When a load that is ready to show raises Revealed.
enum class SceneRevealPolicy : uint8_t
{
    /// In the frame the load is ready.
    AtReady,
    /// When the game calls SceneLoadOperation::Reveal(), such as on a click to continue.
    WhenGameReleases
};

/// One scene load. An aggregate: Load({.Scene = "Scenes/Mission1.scene"}) is the common call.
struct SceneLoadRequest
{
    /// The .scene file, relative to the project's asset root.
    std::filesystem::path Scene;
    /// Single replaces the shown scene. Additive adds the scene to it, and starts only when no other load
    /// is in progress (docs/scripting/scene-loader.mdx, Add a scene to the running world).
    SceneLoadMode Mode = SceneLoadMode::Single;
    /// Runs once the world holds the new scene, before user systems tick against it. It may
    /// refuse the scene with SceneLoadInstantiated::Fail.
    std::function<void(SceneLoadInstantiated&)> OnInstantiated;
    /// AtReady, or WhenGameReleases for a game that shows its own screen until the player continues.
    SceneRevealPolicy Reveal = SceneRevealPolicy::AtReady;
    /// True: the loader pauses the primary world's cameras (Camera::WorldDrawingPaused) at the next frame boundary.
    /// It clears its own pauses at Failed, at Cancelled unless the newer load has the switch, and when the content is
    /// ready, then waits for SceneLoader::kReadyFramesAfterResume ready frames. Keep an opaque cover up until Revealed.
    bool PauseWorldDrawing = false;
};
} // namespace GameEngine
