#pragma once

#include "Components/Spline/SplinePlacement.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineRecipes/SplinePieceMaterial.h"
#include "SplineRecipes/SplineRebuildGate.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

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

// Builds Components::SplinePlacement: drapes a dense centerline of the sibling spline through Scene::RaycastScene,
// spaces tiles by 3D distance along it and keeps them as RuntimeOnlyEntity children of the recipe entity. The scene
// saves only the recipe; the sampled frames are entity-local and take the spline's world transform here.
class SplinePlacementController
{
public:
    // Rebuilds every placement whose spline, transform, recipe or conformed ground changed, once the edit is
    // stable for the settle window. A conforming recipe's first build also waits for its ground (DeferFirstBuild).
    // Retires the tiles of a recipe entity that is gone.
    void Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                float32 deltaSeconds, RecipeBuildStates& states);

    // Destroys every tile and drops all state. Run it against the outgoing world before a clear, because
    // World::Clear restarts entity versions. Update guards the clears that no caller sees
    // (World::DeserializeWorld) with the reset generation.
    void Shutdown(ECS::World& world);

private:
    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // Compared with the component's defaulted memberwise ==, so a future
        // field can never be missed and silently stop rebuilds.
        Components::SplinePlacement Recipe{};
        // The ground the last conform measured (ObservedSurfaceRevision), so a bake or sculpt under a built
        // path places it again. Zero for a ConformMode None recipe, so terrain movement does not delay it.
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct PlacementState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool LoadFailureLogged = false;
        bool CenterlineBudgetLogged = false;
        // Conform misses reported for this entity, so a scene that rebuilds every frame
        // warns when the count moves rather than once per frame.
        uint32 LoggedConformMisses = 0;
        // A rebuild held on a pending BVH (HoldForPendingBvh): not marked built, so it runs again.
        ConformHold Hold;
        // Same on-change contract for the tilt ceiling's held-station count.
        uint32 LoggedTiltClamps = 0;
        std::vector<ECS::EntityHandle> Tiles;
        // The station of each live tile, parallel to Tiles. The mesh pick keys on the station, and dropout
        // can give the same tile count from other stations, so a rebuild compares this sequence, not the count.
        std::vector<uint32> TileStationIndices;
        uint64 VisitStamp = 0;
    };

    // One placement candidate gathered from the ECS query (copied out so the
    // world can be mutated after iteration).
    struct Candidate
    {
        ECS::EntityHandle Entity{};
        Components::SplinePlacement Recipe{};
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        Mathematics::Matrix4x4 WorldMatrix;
    };

    // Returns true when tiles were spawned or destroyed (caller flushes the
    // world's deferred commands once per frame).
    // The candidate of one recipe entity, copied out of the query.
    static Candidate MakeCandidate(ECS::EntityHandle entity, const Components::SplineComponent& spline,
                                   const Components::SplinePlacement& placement, const Components::WorldTransform& transform);

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, PlacementState& state);
    static bool RetireTiles(ECS::World& world, PlacementState& state);

    FlatMap<uint32, PlacementState> m_States; // key: spline entity id
    Engine::Renderer::ModelResolveCache m_ModelCache;
    OverrideMaterialCache m_OverrideMaterials;
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
};

} // namespace GameEngine::SplineRecipes
