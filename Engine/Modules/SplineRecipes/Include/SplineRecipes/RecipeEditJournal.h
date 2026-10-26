#pragma once

namespace GameEngine::ECS { class World; }

namespace GameEngine::SplineRecipes
{

struct SplineGroundCleanup;

// The undo journal as the recipe controllers see it. A destructive transition of a controller (the ground authority
// removes the terrain pieces it made) goes through it, so an undo restores what it removed. A process with no
// journal (the Player, a test) passes null, and the controller applies the transition itself.
class RecipeEditJournal
{
public:
    virtual ~RecipeEditJournal() = default;

    // Applies `cleanup` to `world` and records it as one undo step.
    virtual void RecordGroundCleanup(ECS::World& world, const SplineGroundCleanup& cleanup) = 0;
};

} // namespace GameEngine::SplineRecipes
