#pragma once

#include "Components/Spline/SplineFence.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineRecipes/FenceMitreVariants.h"
#include "SplineRecipes/FencePieceReuse.h"
#include "SplineRecipes/SplineChunkCommit.h"
#include "SplineRecipes/SplinePieceMaterial.h"
#include "SplineRecipes/SplineRebuildGate.h"
#include "Types/FlatMap.h"
#include "Types/ScopedSubscription.h"
#include "Types/Types.h"

#include <atomic>
#include <span>
#include <string>
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

// Builds Components::SplineFence: drapes a dense centerline of the sibling spline with the shared conform ray,
// lays one run per authored segment (the authored points are stations) and keeps the posts and spans as
// RuntimeOnlyEntity children of the recipe entity. The scene saves only the recipe; a hand edit to a piece is lost.
class SplineFenceController
{
public:
    // The span count of each run of the last layout, by the authored point that opens the run; empty for an
    // entity with no layout. The editor's span override remap reads it. Valid until the next Update.
    std::span<const uint32> RunSpanCounts(ECS::EntityHandle entity) const;

    // Rebuilds every fence whose spline, transform, recipe or conformed ground changed, once the edit is stable
    // for the settle window. A conforming recipe's first build also waits for its ground (DeferFirstBuild).
    // Retires the pieces of a recipe entity that is gone.
    void Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                float32 deltaSeconds, RecipeBuildStates& states);

    // Destroys every piece, releases every cut span variant and drops all state. Run it against the outgoing
    // world before a clear, because World::Clear restarts entity versions. Update guards the clears that no
    // caller sees (World::DeserializeWorld) with the reset generation.
    void Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

private:
    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // Compared with the defaulted ==, so a new field always schedules a rebuild. Sanitized (FenceLayout.h),
        // because a NaN would make the recipe unequal to itself and rebuild it forever.
        Components::SplineFence Recipe{};
        // The ground the last conform measured (ObservedSurfaceRevision), so a bake or sculpt under a built
        // fence places it again. Zero for a ConformMode None recipe, so terrain movement does not delay it.
        uint64 SurfaceRevision = 0;
        // FencePoolContentDigest of the span and crest pools: a reimported
        // piece keeps its handle and bounds, so without this nothing here
        // changes and its mitred pieces keep drawing the old cut.
        uint64 PoolContent = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct FenceState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool LoadFailureLogged = false;
        bool CenterlineBudgetLogged = false;
        // Conform misses reported for this entity, so a scene that rebuilds every frame
        // warns when the count moves rather than once per frame.
        uint32 LoggedConformMisses = 0;
        // A rebuild held on a pending BVH (HoldForPendingBvh): not marked built, so it runs again.
        ConformHold Hold;
        // Same on-change contract for the station tilt ceiling and for spans
        // that stood too near vertical to keep a roll of their own.
        uint32 LoggedTiltClamps = 0;
        uint32 LoggedRollBorrowedSpans = 0;
        // The pieces of this recipe in emission order, each with its identity and mesh (FencePieceReuse.h).
        // A rebuild keeps an entity while its identity holds and rewrites its renderer only when its mesh changes.
        std::vector<FencePieceEntity> Pieces;
        // The last layout's span count per run, by the authored point opening
        // it: what a point edit re-addresses this recipe's overrides by.
        std::vector<uint32> RunSpanCounts;
        // The cut variants mitred spans and registered crests draw (fence
        // design section 4c), registered through the chunk commit and released
        // when no piece draws them any more.
        SplineChunkCommit MitreVariants;
        FenceMitreCuts MitreCuts;
        // The pool content digest and the mesh reload count it was taken at,
        // so it is re-read only after the registry reports a reload.
        uint64 PoolContent = 0;
        uint64 PoolContentReloads = ~0ull;
        std::vector<std::string> LoggedValidation;
        uint64 VisitStamp = 0;
    };

    // One fence candidate gathered from the ECS query (copied out so the world
    // can be mutated after iteration).
    struct Candidate
    {
        ECS::EntityHandle Entity{};
        Components::SplineFence Recipe{}; // sanitized at gather time
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        Mathematics::Matrix4x4 WorldMatrix;
    };

    // Returns true when pieces were spawned or destroyed (caller flushes the
    // world's deferred commands once per frame).
    // The candidate of one recipe entity, copied out of the query.
    static Candidate MakeCandidate(ECS::EntityHandle entity, const Components::SplineComponent& spline,
                                   const Components::SplineFence& fence, const Components::WorldTransform& transform);

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, FenceState& state);
    // Destroys the recipe's piece entities; its cut variants stay registered
    // for the pieces about to be spawned in their place.
    static bool RetirePieces(ECS::World& world, FenceState& state);
    // Destroys the pieces and releases the cut variants: the recipe draws
    // nothing any more.
    static bool RetireAll(ECS::World& world, Engine::Renderer::RenderServices* renderServices,
                          FenceState& state);

    FlatMap<uint32, FenceState> m_States; // key: spline entity id
    Engine::Renderer::ModelResolveCache m_ModelCache;
    OverrideMaterialCache m_OverrideMaterials;
    uint64 m_VisitStamp = 0;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
    // Mesh unregisters and in-place reloads the registry has reported. Bumped
    // from the registry's callback, which runs under its table mutex on
    // whichever thread unregisters, so it is an atomic and nothing more.
    std::atomic<uint64> m_MeshReloads{0};
    const Rendering::MeshGPURegistry* m_SubscribedRegistry = nullptr;
    // Declared after the counter it bumps, so it is released first.
    ScopedSubscription m_MeshReloadSubscription;
};

} // namespace GameEngine::SplineRecipes
