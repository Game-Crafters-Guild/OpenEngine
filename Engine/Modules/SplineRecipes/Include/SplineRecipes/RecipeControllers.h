#pragma once

#include "ECS/Entity.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "Types/Types.h"

#include <memory>
#include <span>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::SplineRecipes
{

class SplineExtrudeController;
class SplineFenceController;
class SplineGroundAuthorityController;
class SplinePlacementController;
class SplineWallController;
class RecipeEditJournal;

// Owns every spline recipe controller: the generators (tiles, fence pieces, swept paths and
// roads, walls) and the ground authority that keeps the terrain under a run in step with it.
// Runs on the main thread, in SplineRecipeSystem's exclusive update.
class RecipeControllers
{
public:
    RecipeControllers();
    ~RecipeControllers();

    RecipeControllers(const RecipeControllers&) = delete;
    RecipeControllers& operator=(const RecipeControllers&) = delete;

    // `journal` takes the destructive transitions as undo steps, or is null. With no
    // `renderServices`, no generator runs and every recipe is Failed with kNoRenderDeviceReason.
    void Update(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                RecipeEditJournal* journal, float32 deltaSeconds);

    // Destroys every generated entity and releases every registered mesh. Run it against the
    // OUTGOING world at a scene swap: a cached handle is scoped to the scene it was made in.
    void ReleaseSceneScoped(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

    // The build state of every recipe as the last Update left it.
    const RecipeBuildStates& BuildStates() const { return m_BuildStates; }

    // The fence's last layout's spans per run (SplineFenceController::RunSpanCounts).
    std::span<const uint32> FenceRunSpanCounts(ECS::EntityHandle entity) const;

private:
    void UpdateGenerators(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                          float32 deltaSeconds);

    std::unique_ptr<SplinePlacementController> m_Placement;
    std::unique_ptr<SplineFenceController> m_Fence;
    std::unique_ptr<SplineExtrudeController> m_Extrude;
    std::unique_ptr<SplineWallController> m_Wall;
    std::unique_ptr<SplineGroundAuthorityController> m_GroundAuthority;
    RecipeBuildStates m_BuildStates;
};

} // namespace GameEngine::SplineRecipes
