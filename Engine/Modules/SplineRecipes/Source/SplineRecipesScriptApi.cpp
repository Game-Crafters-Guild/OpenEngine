#include "SplineRecipes/SplineRecipesScriptApi.h"

#include "Core/Engine.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "Logger/Logger.h"
#include "Scripting/AbiMainThread.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/RecipeControllers.h"
#include "SplineRecipes/SplineRecipeSystem.h"

#include <algorithm>
#include <atomic>
#include <string_view>

namespace GameEngine::SplineRecipes
{
namespace
{

const RecipeBuildStates kNoStates;

// The states the running recipe system keeps, or null with no system (no rendering loop).
const RecipeBuildStates* RunningSystemStates()
{
    auto* loop = EngineCore::GetInstance().GetRenderingLoop();
    auto* systems = loop ? loop->GetSystemManager() : nullptr;
    const auto* system = systems ? systems->GetSystem<SplineRecipeSystem>() : nullptr;
    return system ? &system->GetControllers().BuildStates() : nullptr;
}

// With no recipe system, an empty set: every recipe then reads Pending.
const RecipeBuildStates& RunningStates()
{
    const RecipeBuildStates* states = RunningSystemStates();
    return states ? *states : kNoStates;
}

// True on the main thread outside a job, or in a process with no frame loop. Otherwise logs once
// for `function` and returns false.
bool AcceptsThread(std::atomic<bool>& refused, std::string_view function)
{
    if (ScriptingAbi::OnEngineMainThreadFor(EngineCore::GetInstance()))
        return true;
    if (!refused.exchange(true, std::memory_order_relaxed))
        Logger::Log::Error("SplineRecipes::{} was called off the main thread and was refused: the recipe "
                           "system writes the build states on the main thread. Call it on the main thread, "
                           "outside a job.",
                           function);
    return false;
}

} // namespace

RecipeStatus StateOf(ECS::World& world, ECS::EntityHandle recipe)
{
    static std::atomic<bool> refused{false};
    if (!AcceptsThread(refused, "StateOf"))
        return {recipe, BuildState::Pending};
    return RunningStates().StateOf(world, recipe);
}

bool IsSceneSettled(ECS::World& world)
{
    static std::atomic<bool> refused{false};
    if (!AcceptsThread(refused, "IsSceneSettled"))
        return false;
    return RunningStates().IsSceneSettled(world);
}

std::int32_t Failures(ECS::World& world, std::span<RecipeStatus> failures)
{
    static std::atomic<bool> refused{false};
    if (!AcceptsThread(refused, "Failures"))
        return 0;
    const std::span<const RecipeStatus> all = RunningStates().Failures(world);
    std::copy_n(all.begin(), std::min(all.size(), failures.size()), failures.begin());
    return static_cast<std::int32_t>(all.size());
}

std::string ReasonOf(ECS::World& world, ECS::EntityHandle recipe)
{
    static std::atomic<bool> refused{false};
    if (!AcceptsThread(refused, "ReasonOf"))
        return {};
    return std::string(RunningStates().ReasonOf(world, recipe));
}

bool IsSceneSettledAtLastUpdate(ECS::World& world)
{
    const RecipeBuildStates* states = RunningSystemStates();
    return states ? states->IsSceneSettledAtLastUpdate(world) : kNoStates.IsSceneSettled(world);
}

} // namespace GameEngine::SplineRecipes
