#pragma once

#include "Engine/Loading/SceneLoadRequest.h"
#include "Events/Event.h"
#include "JobSystem/TaskHandle.h"
#include "Scene/AssetManagerSceneResolver.h"
#include "Scene/SceneIO.h"
#include "SceneLoadTracker/SceneLoadProgress.h"
#include "SceneLoadTracker/SceneLoadTracker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace GameEngine
{
// The state that a SceneLoadOperation shares with the loader. Main thread, except the
// read job: it writes Read and ReadTicket, then sets ReadDone, and touches nothing else.
struct SceneLoadState
{
    enum class Step : uint8_t
    {
        Queued,
        WaitingForScan,
        Reading,
        WaitingForReady,
        // PauseWorldDrawing: the world draws again, and the load waits for ready frames in a row.
        WaitingForReadyFramesAfterResume,
        WaitingForReveal, // ready; SceneRevealPolicy::WhenGameReleases waits for Reveal()
        Revealed,
        Failed,
        Cancelled
    };

    SceneLoadRequest Request;
    std::filesystem::path SceneFile; // absolute
    Step Current = Step::Queued;
    std::chrono::steady_clock::time_point Started{};

    // The read job holds the state, so the resolver and the options outlive it.
    std::unique_ptr<Scene::AssetManagerSceneResolver> Resolver;
    Scene::SceneReadOptions ReadOptions;
    // The loader empties Read.Document once the scene is instantiated, or once the load is
    // cancelled and its read has ended.
    Scene::SceneReadResult Read;
    SceneLoadTracker::WorkTicket ReadTicket;
    std::atomic<bool> ReadDone{false};
    JobSystem::TaskHandle ReadTask;
    // The bar from Started, and what the operation's getters read. The loader updates them each
    // frame while the load is current. ReadyToShow stays true once the loader set it.
    SceneLoadProgress Progress{std::chrono::steady_clock::time_point{}};
    double SecondsRemaining = 0.0;
    bool ReadyToShow = false;
    // In WaitingForReadyFramesAfterResume: the tracker-ready frames in a row since the resume.
    int ReadyFramesInARow = 0;
    // Set by SceneLoadOperation::Reveal(). The loader reads it only while the load is current.
    bool RevealRequested = false;

    Event<> Ready;
    Event<> Revealed;
    Event<> Cancelled;
    Event<std::string_view> Failed;
};
} // namespace GameEngine
