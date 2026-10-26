#pragma once

#include "Components/Spline/SplineWall.h"
#include "ECS/ECS.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineRecipes/SplineChunkCommit.h"
#include "SplineRecipes/SplinePieceMaterial.h"
#include "SplineRecipes/SplineRebuildGate.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Components
{
struct SplineComponent;
struct WorldTransform;
} // namespace GameEngine::Components
namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::SplineRecipes
{

class RecipeBuildStates;

// Builds Components::SplineWall: drapes the sibling spline with the shared conform ray, sweeps the wall
// (SplineWallBuild.h) and commits its chunk meshes under stable keys (SplineChunkCommit.h), so a rebuild
// churns no entities. Its rebuild rules are the extrude's.
class SplineWallController
{
public:
    // Rebuilds every wall whose spline, transform, recipe or conformed ground changed, once the edit is stable
    // for the settle window, and retires the chunks of a wall entity that is gone.
    void Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                float32 deltaSeconds, RecipeBuildStates& states);

    // Releases every chunk mesh and destroys every chunk entity. Run it against the outgoing world before a
    // scene swap clears it, because a chunk handle that survives a clear can match a new entity.
    void Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

private:
    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // Compared with the component's defaulted memberwise ==, so a future
        // field can never be missed and silently stop rebuilds.
        Components::SplineWall Recipe{};
        // The ground the last rebuild measured (ObservedSurfaceRevision); zero
        // for a wall that does not conform.
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct WallState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool CenterlineBudgetLogged = false;
        uint32 LoggedConformMisses = 0;
        // A rebuild held on a pending BVH (HoldForPendingBvh): not marked built, so it runs again.
        ConformHold Hold;
        // Validation lines last logged for this wall, so they log once per
        // change rather than once per rebuild (ReportRecipeValidation).
        std::vector<std::string> LoggedValidation;
        SplineChunkCommit Chunks;
        // Why the last build made no chunk. Empty when it made one.
        std::string_view EmptyReason;
        uint64 VisitStamp = 0;
    };

    struct Candidate
    {
        ECS::EntityHandle Entity{};
        // As authored, for the validation a sanitized value no longer shows.
        Components::SplineWall Authored{};
        // Sanitized: what is compared and built.
        Components::SplineWall Recipe{};
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        Mathematics::Matrix4x4 WorldMatrix;
    };

    // The candidate of one recipe entity, copied out of the query.
    static Candidate MakeCandidate(ECS::EntityHandle entity, const Components::SplineComponent& spline,
                                   const Components::SplineWall& recipe, const Components::WorldTransform& transform);

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, WallState& state);
    // Binds the wall's material and puts its chunk meshes on screen through the
    // chunk commit.
    bool Commit(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                const Candidate& candidate, WallState& state, const std::vector<Mesh>& meshes);
    // Drops all cached state after a world reset WITHOUT destroying anything
    // (SplineChunkCommit::DropAfterWorldReset).
    void DropStateAfterWorldReset(Engine::Renderer::RenderServices* renderServices);

    FlatMap<uint32, WallState> m_States; // key: spline entity id
    // This frame's walls, kept between frames so collecting them allocates only
    // when the count grows.
    std::vector<Candidate> m_Candidates;
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
    bool m_DefaultMaterialRegistered = false;
    OverrideMaterialCache m_OverrideMaterials;
};

} // namespace GameEngine::SplineRecipes
