#pragma once

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{

struct SplineGroundEdit;
class SplineGroundAuthorityController;

// The undo journal as the recipe controllers see it. A controller applies a transition (the ground authority adds,
// rewrites or removes the terrain pieces it makes) and records it here, so one undo reverts the edit that caused it
// and the transition. A process with no journal (the Player, a test) passes null.
class RecipeEditJournal
{
public:
    virtual ~RecipeEditJournal() = default;

    virtual void BeginGroundUpdate(ECS::World& world, SplineGroundAuthorityController& controller) = 0;
    virtual void EndGroundUpdate() = 0;

    // Records `edit`, which the controller applied to `world`, in the undo step of the edit that caused it.
    virtual void RecordGroundEdit(ECS::World& world, const SplineGroundEdit& edit) = 0;
};

} // namespace GameEngine::SplineRecipes
