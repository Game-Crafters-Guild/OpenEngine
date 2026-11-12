#include "SceneLoadTracker/SceneLoaderScriptApi.h"

#include "Core/Engine.h"
#include "Engine/Loading/SceneLoader.h"
#include "Types/PathUtils.h"

#include <format>
#include <stdexcept>

namespace GameEngine::Loading::Script
{
namespace
{
static_assert(static_cast<std::uint8_t>(SceneRevealPolicy::AtReady) ==
              static_cast<std::uint8_t>(GameEngine::SceneRevealPolicy::AtReady));
static_assert(static_cast<std::uint8_t>(SceneRevealPolicy::WhenGameReleases) ==
              static_cast<std::uint8_t>(GameEngine::SceneRevealPolicy::WhenGameReleases));

// The newest load that a script started. A script's Load cancels the load before it, so the binding
// keeps only this one. The operation shares the load's state, so it stays valid after the load ends.
struct NewestScriptLoad
{
    std::uint64_t Id = 0;
    GameEngine::SceneLoadOperation Operation;
};

// Main thread: the generated binding refuses a call from any other thread.
NewestScriptLoad& Newest()
{
    static NewestScriptLoad newest;
    return newest;
}

// The loader's operation for `load`. Id 0 gives an operation of no load, which answers 0 and false.
// Throws for a load that a newer script Load replaced, and for a number that no Load returned.
GameEngine::SceneLoadOperation& Find(SceneLoadOperation load)
{
    static GameEngine::SceneLoadOperation noLoad;
    NewestScriptLoad& newest = Newest();
    if (load.Id == 0)
        return noLoad;
    if (load.Id == newest.Id)
        return newest.Operation;
    if (load.Id < newest.Id)
        throw std::invalid_argument(std::format(
            "load {} was replaced by load {}, which a newer Load started. Read the operation that the newest Load returned",
            load.Id, newest.Id));
    throw std::invalid_argument(std::format("no Load returned load {}", load.Id));
}
} // namespace

SceneLoadOperation Load(std::string_view scene, SceneRevealPolicy reveal, bool pauseWorldDrawing)
{
    NewestScriptLoad& newest = Newest();
    newest.Operation = EngineCore::GetInstance().GetSceneLoader().Load(
        {.Scene = PathFromUtf8(scene),
         .Reveal = static_cast<GameEngine::SceneRevealPolicy>(reveal),
         .PauseWorldDrawing = pauseWorldDrawing});
    ++newest.Id;
    return {.Id = newest.Id};
}

bool IsLoading()
{
    return EngineCore::GetInstance().GetSceneLoader().IsLoading();
}

float Progress(SceneLoadOperation load)
{
    return Find(load).Progress();
}

bool IsProgressShown(SceneLoadOperation load)
{
    return Find(load).IsProgressShown();
}

double EstimatedSecondsRemaining(SceneLoadOperation load)
{
    return Find(load).EstimatedSecondsRemaining();
}

bool IsReadyToShow(SceneLoadOperation load)
{
    return Find(load).IsReadyToShow();
}

void Reveal(SceneLoadOperation load)
{
    Find(load).Reveal();
}
} // namespace GameEngine::Loading::Script
