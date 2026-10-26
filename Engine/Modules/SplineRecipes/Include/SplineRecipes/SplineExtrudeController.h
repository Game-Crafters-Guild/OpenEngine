#pragma once

#include "AssetCore/GUID.h"
#include "Components/Spline/SplineExtrude.h"
#include "ECS/ECS.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineRecipes/SplineChunkCommit.h"
#include "SplineRecipes/SplineFillRebuild.h"
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

// Builds Components::SplineExtrude: drapes a dense centerline of the sibling spline with the shared conform ray,
// then sweeps the cross-section along it (Channel) or fills the water's area over the terrain (FitToBanks). Each
// chunk is a generated mesh under a stable key, registered again in place, so a rebuild churns no entities.
class SplineExtrudeController
{
public:
    // Rebuilds every extrude whose spline, transform, recipe or conformed ground changed, once the edit is
    // stable for the settle window, and retires the chunks of a recipe entity that is gone.
    void Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                float32 deltaSeconds, RecipeBuildStates& states);

    // Releases every chunk mesh and destroys every chunk entity. Run it against the outgoing world before a
    // clear: the registry does not refcount, and World::Clear restarts entity versions. Update guards the
    // clears that no caller sees (World::DeserializeWorld) with the reset generation.
    void Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

private:
    // The recipe fields that only change how the chunks draw. A change rebinds the chunk
    // renderers and runs no rebuild.
    struct ExtrudeLook
    {
        Components::MaterialRef Material;
        bool CastShadows = true;
        bool ReceiveShadows = true;

        bool operator==(const ExtrudeLook&) const = default;
    };

    // Everything a rebuild depends on; a change in any part schedules one.
    struct ObservedState
    {
        SplineObservedInputs Spline{};
        // The recipe with its look fields at their defaults (ExtrudeLook), compared with the
        // component's defaulted memberwise ==, so a new field always schedules a rebuild.
        Components::SplineExtrude Recipe{};
        // The ground the last rebuild measured (ObservedSurfaceRevision), so a bake or sculpt under a built
        // path rebuilds it. Zero for a recipe that reads no ground (Components::ReadsSurface); a FitToBanks
        // recipe reads its banks in every ConformMode.
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    struct ExtrudeState
    {
        SplineRebuildGate<ObservedState> Gate;
        bool CenterlineBudgetLogged = false;
        uint32 LoggedConformMisses = 0;
        // A rebuild held on a pending BVH (HoldForPendingBvh): not marked built, so it runs again.
        ConformHold Hold;
        // The chunks whose first build waits for a model near them. The gate is marked built, so
        // only a release of a waiting chunk runs the next rebuild.
        ChunkModelHold Models;
        // The meshes under a Scene-conform run, read again only when the scene TLAS changes.
        SceneConformDigest SceneDigest;
        // Validation lines last logged for this recipe (a retired profile), so
        // they log once per change rather than once per frame.
        std::vector<std::string> LoggedValidation;
        // Last reported water-fill state, so the diagnostics log once per change
        // rather than once per rebuild.
        SplineGeometry::SplineFillDiagnostics LastFillDiagnostics{};
        WaterFillOutcome LastFillOutcome = WaterFillOutcome::Built;
        SplineChunkCommit Chunks;
        // The look the chunk renderers carry.
        ExtrudeLook AppliedLook;
        // Why the last build made no chunk. Empty when it made one.
        std::string_view EmptyReason;
        uint64 VisitStamp = 0;
    };

    struct Candidate
    {
        ECS::EntityHandle Entity{};
        Components::SplineExtrude Recipe{};
        // Saved with the retired Rectangle profile: built into nothing and
        // reported (CarriesRetiredWallProfile).
        bool RetiredProfile = false;
        uint32 SplineDataIndex = 0;
        uint32 SplineDataGeneration = 0;
        Mathematics::Matrix4x4 WorldMatrix;
    };

    // The candidate of one recipe entity, copied out of the query.
    static Candidate MakeCandidate(ECS::World& world, ECS::EntityHandle entity, const Components::SplineComponent& spline,
                                   const Components::SplineExtrude& recipe, const Components::WorldTransform& transform);

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                 const Candidate& candidate, ExtrudeState& state);
    // Binds the recipe's material and commits a built set of chunk meshes (SplineChunkCommit.h).
    // A template only because the two emitters return two mesh types of the same shape.
    template <typename MeshT>
    bool CommitChunkMeshes(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                           const Candidate& candidate, ExtrudeState& state,
                           const std::vector<MeshT>& chunkMeshes);

    // The pending conform targets of `world`, collected once per Update on the first call.
    std::span<const PendingConformTarget> PendingTargets(ECS::World& world);

    // The renderer every chunk of `recipe` carries, with the recipe's material when it is
    // renderable and the minted default otherwise.
    Components::MeshRenderer ChunkRenderer(Engine::Renderer::RenderServices& renderServices,
                                           const Components::SplineExtrude& recipe);

    // Drops all cached state after a world reset WITHOUT destroying anything
    // (SplineChunkCommit::DropAfterWorldReset).
    void DropStateAfterWorldReset(Engine::Renderer::RenderServices* renderServices);

    FlatMap<uint32, ExtrudeState> m_States; // key: spline entity id
    uint64 m_VisitStamp = 0;
    std::vector<PendingConformTarget> m_PendingTargets;
    bool m_PendingTargetsCollected = false;
    // World::GetLifecycleResetGeneration() as of the last Update.
    uint64 m_LastWorldResetGeneration = 0;
    bool m_DefaultMaterialRegistered = false;
    OverrideMaterialCache m_OverrideMaterials;
};

} // namespace GameEngine::SplineRecipes
