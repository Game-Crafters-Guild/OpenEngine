#pragma once

// The SplineRecipes script API: whether each spline recipe has built, and whether every recipe of
// the scene has. The marked declarations are generated into the C# binding. Call them on the main
// thread outside a job: a call from another thread is refused, logs once, and returns the default.

#include "ECS/ECS.h"

#include <cstdint>
#include <span>
#include <string>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::SplineRecipes
{

/// Where a spline recipe's build stands.
// @ge-script
enum class BuildState : std::uint8_t
{
    /// The recipe has not built yet, or an edit to it waits for its rebuild.
    Pending = 0,
    /// The recipe waits for something it builds on, such as the terrain under it. ReasonOf says what.
    Held = 1,
    /// The recipe's output matches the recipe. When the output is empty, ReasonOf says why.
    Built = 2,
    /// The recipe cannot build. ReasonOf says why and how to fix it.
    Failed = 3,
};

/// A recipe entity and its build state.
// @ge-script
struct RecipeStatus
{
    /// The entity that carries the recipe.
    ECS::EntityHandle Recipe{};
    /// Where its build stands.
    BuildState State = BuildState::Pending;
};

/// The build state of `recipe`. A recipe the recipe system has not updated yet is Pending. An
/// entity with no recipe is Built: it has nothing to build.
// @ge-script @ge-context
RecipeStatus StateOf(ECS::World& world, ECS::EntityHandle recipe);

/// True when no recipe in the world is Pending or Held: every recipe is Built or Failed. A world
/// with no recipe is settled. Poll it each frame: an edit or a late asset can unsettle it again.
// @ge-script @ge-context
bool IsSceneSettled(ECS::World& world);

/// Writes every Failed recipe into `failures` and returns how many there are, also when that is
/// more than `failures` holds.
// @ge-script @ge-context
std::int32_t Failures(ECS::World& world, std::span<RecipeStatus> failures);

/// Why `recipe` is Held or Failed, or why a Built recipe made nothing, in words that name the
/// fix. Empty for a recipe that is Pending, or Built with output.
// @ge-script @ge-context
std::string ReasonOf(ECS::World& world, ECS::EntityHandle recipe);

/// IsSceneSettled as the recipe system found it at the end of its last update of `world`, or false
/// before that update. For a system of the schedule, which can run on a job: it reads one value the
/// recipe system's exclusive update writes. With no recipe system, it is IsSceneSettled.
bool IsSceneSettledAtLastUpdate(ECS::World& world);

} // namespace GameEngine::SplineRecipes
