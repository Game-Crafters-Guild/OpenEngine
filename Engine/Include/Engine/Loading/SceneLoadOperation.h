#pragma once

#include "Events/Event.h"

#include <memory>
#include <string_view>

namespace GameEngine
{
struct SceneLoadState;

/// A handle to one scene load, cheap to copy: copies share the load's state, its events
/// included. Main thread. Each event is raised once, and a load raises exactly one of
/// Revealed, Failed and Cancelled, or none when the engine shuts down first. Ready comes
/// before Revealed, and a ready load can still be cancelled. Event::operator+= returns a
/// subscription that unsubscribes when it is destroyed: keep it.
class SceneLoadOperation
{
  public:
    /// An operation that belongs to no load: its events are never raised.
    SceneLoadOperation() = default;

    /// The bar of a loading screen, 0 to 1 (docs/scripting/scene-loader.mdx, Show load progress).
    /// It never goes down, rises at most 0.1 in any 250 ms before ready, and is 1 from ready.
    float Progress() const;
    /// True from the frame the load has run 1 s with an estimated total of 2.5 s or more, and it
    /// stays true. A shorter load stays false: a game can skip the bar for it.
    bool IsProgressShown() const;
    /// The estimate of the seconds until the new scene is ready to show, at the last frame the
    /// load ran. 0 once it is ready, and from the frame a PauseWorldDrawing load resumes the world.
    double EstimatedSecondsRemaining() const;
    /// True from the frame of Ready.
    bool IsReadyToShow() const;

    /// Releases a SceneRevealPolicy::WhenGameReleases load. The loader raises Revealed at the next frame
    /// boundary, or in the frame of Ready when the call comes before ready or from a Ready handler.
    /// It does nothing for an AtReady load and for a load that was revealed, failed or was cancelled.
    void Reveal();

    /// The new scene is in the world, every Frame work item the tracker holds is done, and
    /// Progress() is 1. With SceneLoadRequest::PauseWorldDrawing, the world also drew again and the
    /// tracker was ready for SceneLoader::kReadyFramesAfterResume frames in a row. Raised in the frame
    /// the load is ready, under either SceneRevealPolicy.
    Event<>& Ready() const;
    /// Raised in the frame of Ready for SceneRevealPolicy::AtReady, and after Reveal() for
    /// SceneRevealPolicy::WhenGameReleases.
    Event<>& Revealed() const;
    /// A newer SceneLoadMode::Single load replaced this one before it was revealed.
    Event<>& Cancelled() const;
    /// The load stopped with the reason: the scene could not be read (the world that was
    /// shown is untouched), could not be instantiated, or OnInstantiated refused it. An
    /// Additive load also fails, before it touches the world, when another load is in progress.
    Event<std::string_view>& Failed() const;

  private:
    friend class SceneLoader;
    explicit SceneLoadOperation(std::shared_ptr<SceneLoadState> state);

    std::shared_ptr<SceneLoadState> m_State;
};
} // namespace GameEngine
