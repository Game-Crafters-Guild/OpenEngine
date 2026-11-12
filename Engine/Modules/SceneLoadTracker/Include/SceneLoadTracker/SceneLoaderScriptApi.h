#pragma once

// The SceneLoader script API: how a game's scripts load a scene and show their own loading
// screen (docs/scripting/scene-loader.mdx). Every marked declaration is generated into the C#
// binding (SceneLoader); a native script calls the same functions. The bodies are in Engine
// (Engine/Source/Engine/Loading/SceneLoaderScriptApi.cpp), beside the loader they ask.

#include <cstdint>
#include <string_view>

namespace GameEngine::Loading::Script
{

/// When a load that is ready to show is revealed: the new scene shows and its game starts.
// @ge-script
enum class SceneRevealPolicy : std::uint8_t
{
    /// In the frame the load is ready.
    AtReady = 0,
    /// When the game calls Reveal, such as on a click to continue.
    WhenGameReleases = 1,
};

/// One scene load that a script started. Load returns it, and the other functions read it.
// @ge-script
struct SceneLoadOperation
{
    /// The load's number. 0 is no load: the functions answer 0 or false for it and Reveal does nothing.
    std::uint64_t Id = 0;
};

/// Replaces the shown scene with `scene`, a .scene file relative to the project's asset root. The load starts at
/// the next frame boundary, never inside a system. A newer Load cancels this one, and the functions then refuse this
/// operation. `pauseWorldDrawing` pauses the world until the scene has drawn complete frames, or until the load fails
/// or a newer Load without `pauseWorldDrawing` cancels it (SceneLoadRequest::PauseWorldDrawing).
// @ge-script
SceneLoadOperation Load(std::string_view scene, SceneRevealPolicy reveal = SceneRevealPolicy::AtReady,
                        bool pauseWorldDrawing = false);

/// True from Load until the newest load is revealed, fails or is cancelled. A load that native code
/// started counts too.
// @ge-script
bool IsLoading();

/// The bar of a loading screen, 0 to 1. It never goes down, rises at most 0.1 in any 250 ms before the
/// load is ready to show, and is 1 from then on.
// @ge-script
float Progress(SceneLoadOperation load);

/// True from the frame the load has run 1 s with an estimated total of 2.5 s or more, and it stays true.
/// A shorter load stays false: a game can skip the bar for it.
// @ge-script
bool IsProgressShown(SceneLoadOperation load);

/// The estimate of the seconds until the new scene is ready to show, at the last frame the load ran.
/// 0 once it is ready, and from the frame a pauseWorldDrawing load resumes the world.
// @ge-script
double EstimatedSecondsRemaining(SceneLoadOperation load);

/// True from the frame the new scene is ready to show: it is in the world, the work it needs is done,
/// and Progress is 1.
// @ge-script
bool IsReadyToShow(SceneLoadOperation load);

/// Reveals a WhenGameReleases load: in the frame it is ready when the call comes before that, else at
/// the next frame boundary. It does nothing for an AtReady load and for a load that was revealed,
/// failed or was cancelled.
// @ge-script
void Reveal(SceneLoadOperation load);

} // namespace GameEngine::Loading::Script
