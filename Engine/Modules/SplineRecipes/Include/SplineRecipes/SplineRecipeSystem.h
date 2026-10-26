#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine::ECS { class SystemScheduleBuilder; class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::SplineRecipes
{

class RecipeControllers;
class RecipeEditJournal;

// Runs the spline recipe controllers once a frame, in the editor, the Player and a game alike.
// Exclusive, on the main thread: the controllers register meshes and make and destroy entities.
// It runs after TerrainModifiers and SplineExtraction, so a build reads this frame's bake and caches.
class SplineRecipeSystem : public ECS::ISystem
{
public:
    explicit SplineRecipeSystem(Engine::Renderer::RenderServices* renderServices);
    ~SplineRecipeSystem() override;

    SplineRecipeSystem(const SplineRecipeSystem&) = delete;
    SplineRecipeSystem& operator=(const SplineRecipeSystem&) = delete;

    const char* GetName() const override { return "SplineRecipeSystem"; }
    bool RequiresExclusiveUpdate() const override { return true; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // The journal the controllers write their destructive transitions to as undo
    // steps; null, the default, in a process with no undo. Its owner sets null
    // again before the journal is destroyed.
    void SetEditJournal(RecipeEditJournal* journal);

    const RecipeControllers& GetControllers() const;

    // Destroys what the recipes generated in `world` and releases their meshes
    // (RecipeControllers::ReleaseSceneScoped). Run against the OUTGOING world
    // at a scene swap, before the incoming scene loads.
    void ReleaseSceneScoped(ECS::World& world);

private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    std::unique_ptr<RecipeControllers> m_Controllers;
    RecipeEditJournal* m_EditJournal = nullptr;
};

// Adds SplineRecipeSystem to an external schedule builder, in the Extraction
// phase after TerrainModifiers and SplineExtraction.
void AddSplineRecipeSystemsToSchedule(ECS::SystemScheduleBuilder& b,
                                      Engine::Renderer::RenderServices* renderServices);

} // namespace GameEngine::SplineRecipes
