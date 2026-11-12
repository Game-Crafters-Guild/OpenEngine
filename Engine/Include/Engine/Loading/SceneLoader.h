#pragma once

#include "Engine/Loading/SceneLoadOperation.h"
#include "Engine/Loading/SceneLoadRequest.h"
#include "SceneLoadTracker/SceneLoadTracker.h"

#include <chrono>
#include <memory>
#include <vector>

namespace GameEngine::ECS
{
struct EntityHandle;
}

namespace GameEngine
{
class EngineCore;
struct SceneLoaderTestAccess;

/// Loads a scene into the engine's primary world: the one way to replace the scene that a game
/// shows or to add a scene to it (docs/scripting/scene-loader.mdx). It reads the scene on a worker
/// and instantiates it on the main thread. A Single load clears the world first. When the tracker is
/// ready to show, the load raises Ready and reveals as its SceneRevealPolicy says (a PauseWorldDrawing
/// load first resumes the world and counts ready frames). EngineCore::GetSceneLoader() returns it. Main thread.
class SceneLoader
{
  public:
    /// The tracker-ready frames in a row that a PauseWorldDrawing load waits for after it resumes the
    /// world. A miss starts the count again. Three cover a feature that asks for its next pipeline up
    /// to two frames after its last one was built.
    static constexpr int kReadyFramesAfterResume = 3;

    explicit SceneLoader(SceneLoadTracker& tracker);
    ~SceneLoader();
    SceneLoader(const SceneLoader&) = delete;
    SceneLoader& operator=(const SceneLoader&) = delete;

    /// Replaces the primary world's scene with request.Scene, or adds it to the world for
    /// SceneLoadMode::Additive. The load starts at the next frame boundary, never inside a system.
    /// A newer Single load cancels this one. An Additive load fails when another load is in progress.
    SceneLoadOperation Load(SceneLoadRequest request);
    /// True while a load of either mode is queued or in progress, also while it waits for Reveal().
    bool IsLoading() const;
    /// True while a Single load is queued or in progress, also while it waits for Reveal(). False
    /// for an Additive load. A game that pauses its match while the shown scene is replaced reads it.
    bool IsReplacingScene() const;

  private:
    // EngineCore calls Tick and Shutdown. Tests step a loader through SceneLoaderTestAccess.
    friend class EngineCore;
    friend struct SceneLoaderTestAccess;

    // The main-thread work one frame may do while a load runs. A wait for the GPU is not work.
    static constexpr std::chrono::microseconds kFrameBound{33000};

    // Once per frame at the frame boundary, after the tracker's Tick. `gpuWait` is the time the last
    // frame's main thread waited for the GPU to free a frame slot and give the next swapchain image.
    void Tick(std::chrono::steady_clock::time_point now, std::chrono::microseconds gpuWait);
    // At shutdown: drops every load without an event and waits for every read in flight,
    // the cancelled loads' included.
    void Shutdown();
    static bool IsReadInFlight(const SceneLoadState& state);
    // Takes the loads queued since the last frame, in call order. A Single load replaces the current
    // load and the queued ones before it. An Additive load fails when another load is current or starts.
    void StartQueued();
    void Begin(const std::shared_ptr<SceneLoadState>& state);
    void Advance();
    void StartRead(const std::shared_ptr<SceneLoadState>& state);
    void Instantiate();
    // Sets WorldDrawingPaused on each camera of the primary world that draws, and records it.
    void PauseWorldDrawing();
    // Clears WorldDrawingPaused on the recorded cameras that still exist, and empties the record.
    void ResumeWorldDrawing();
    // A PauseWorldDrawing load: counts the tracker-ready frames in a row after the resume.
    void CountReadyFramesAfterResume();
    // Once per frame for the current load: its estimate and its bar.
    void UpdateProgress(std::chrono::steady_clock::time_point now);
    // Once per frame for a current load that is ready, after its bar is 1: raises Ready in the
    // first such frame, then reveals the load at AtReady or after SceneLoadOperation::Reveal().
    void UpdateReveal();
    void Reveal();
    void Fail(std::string_view reason);
    void Cancel(const std::shared_ptr<SceneLoadState>& state);
    // The resolve slice widens only while IsReplacingScene(): an Additive load runs while the game
    // plays and draws the world.
    void UpdateBudget(std::chrono::steady_clock::time_point now, std::chrono::microseconds tickWork,
                      std::chrono::microseconds gpuWait);

    SceneLoadTracker& m_Tracker;
    SceneLoadTracker::Category m_ReadCategory;
    SceneLoadTracker::Category m_InstantiateCategory;
    std::shared_ptr<SceneLoadState> m_Current;
    std::vector<std::shared_ptr<SceneLoadState>> m_Queued; // in call order; StartQueued takes them
    std::vector<std::shared_ptr<SceneLoadState>> m_Draining; // cancelled, their read still in flight
    // The primary world's cameras that the current load paused or took over. Every end of the load resumes them.
    std::vector<ECS::EntityHandle> m_PausedCameras;
    std::chrono::steady_clock::time_point m_LastTick{};
    std::chrono::microseconds m_LastTickWork{0};
    bool m_BudgetActive = false;
};
} // namespace GameEngine
