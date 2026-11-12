#pragma once

#include "Components/AssetRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplinePaving.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector2.h"
#include "SplineRecipes/ConformGroundWatch.h"
#include "SplineRecipes/Paving/PavingApproach.h"
#include "SplineRecipes/Paving/PavingSources.h"
#include "SplineRecipes/Paving/PavingRoadTrim.h"
#include "SplineRecipes/Paving/PavingStatus.h"
#include "SplineRecipes/RecipeBuildStates.h"
#include "SplineRecipes/SplineChunkCommit.h"
#include "SplineRecipes/SplinePieceMaterial.h"
#include "SplineRecipes/SplineRebuildGate.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
enum class MaterialPatternSymmetry : uint8_t;
}
namespace GameEngine::ECS
{
class World;
}
namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::SplineRecipes
{

// Builds every SplinePaving of a world: one lid and one border chunk per region, on the ground under
// the owner's target, as runtime-only chunk entities under the owner. The gate is world-wide: a
// geometry edit rebuilds every region after the settle, and a look edit rebinds the chunks.
class SplinePavingController
{
public:
    void Update(ECS::World& world, Engine::Renderer::RenderServices& renderServices, float32 deltaSeconds,
                RecipeBuildStates& states);
    void Shutdown(ECS::World& world, Engine::Renderer::RenderServices* renderServices);

    // Per road, the arc length its regions took over at the last rebuild and the Open edges there:
    // what the road's own strip skips and caps at.
    const PavingRoadTrims& RoadTrims() const { return m_RoadTrims; }

    // The approaches of every region an Area's paving owns, at the last rebuild.
    std::span<const PavingApproach> Approaches() const { return m_Approaches; }
    // Rises each time Approaches() is set.
    uint64 ApproachesVersion() const { return m_ApproachesVersion; }

    // The roads whose strips wait while the regions wait for picking data: every road that the
    // last trims or the waiting rebuild's trims cut. Sorted by entity id; empty when no region waits.
    std::span<const uint32> HeldRoads() const { return m_HeldRoads; }

    // What `entity`'s paving did at the last rebuild, or null before its first.
    const PavingStatus* StatusOf(ECS::EntityHandle entity) const;

    // Fills `recipe`'s empty VergeMaterial with the curb material of the first paving along `road`
    // that paves it (its BorderMaterial, else its Material). A set VergeMaterial stays.
    void PrefillVergeMaterial(const ECS::World& world, ECS::EntityHandle road, Components::SplineExtrude& recipe) const;

private:
    // One paving as a rebuild depends on it: its entity, its recipe's geometry key, and what the seams
    // read of its material (the pattern's symmetry tag and the albedo tiling).
    struct ObservedRecipe
    {
        uint32 Entity = 0;
        Components::SplinePaving Geometry{};
        MaterialPatternSymmetry PatternSymmetry{};
        Mathematics::Vector2 PatternTilesPerUV{1.0f, 1.0f};

        bool operator==(const ObservedRecipe&) const = default;
    };

    struct ObservedState
    {
        std::vector<ObservedRecipe> Recipes;
        std::vector<PavingSplineObservation> Splines;
        std::vector<uint64> Junctions;
        uint64 SurfaceRevision = 0;

        bool operator==(const ObservedState&) const = default;
    };

    // What an owner's chunks were bound with.
    struct OwnerLook
    {
        Components::MaterialRef Material;
        Components::MaterialRef BorderMaterial;
        bool CastShadows = true;
        bool ReceiveShadows = true;

        bool operator==(const OwnerLook&) const = default;
    };

    // The courses of one material along the seams of an owner's regions, one slot per region.
    struct CourseChunks
    {
        Components::MaterialRef Material;
        SplineChunkCommit Chunks;
    };

    // The chunks of the regions one paving owns: one lid, one border and one crossfade band slot per
    // region, and the courses per course material.
    struct OwnerState
    {
        SplineChunkCommit Lid;
        SplineChunkCommit Border;
        SplineChunkCommit Band;
        std::vector<CourseChunks> Courses;
        OwnerLook Look;
        bool Built = false;
    };

    // The meshes of an owner's regions in region order, one per slot.
    struct OwnerMeshes
    {
        std::vector<Mesh> Lids;
        std::vector<Mesh> Borders;
        std::vector<Mesh> Bands;
        std::vector<std::pair<Components::MaterialRef, std::vector<Mesh>>> Courses;
    };

    bool Rebuild(ECS::World& world, Engine::Renderer::RenderServices& renderServices, const PavingWorld& paved,
                 RecipeBuildStates& states);
    bool CommitOwner(ECS::World& world, Engine::Renderer::RenderServices& renderServices,
                     const PavingCandidate& owner, OwnerState& state, OwnerMeshes& meshes);
    void Rebind(ECS::World& world, Engine::Renderer::RenderServices& renderServices, const PavingWorld& paved);
    Components::MeshRenderer RendererFor(Engine::Renderer::RenderServices& renderServices,
                                         const Components::MaterialRef& material, const OwnerLook& look);
    // The renderer of a seam band: `material` with the UV1 crossfade on, registered under a GUID derived
    // from the material it binds.
    Components::MeshRenderer BandRendererFor(Engine::Renderer::RenderServices& renderServices,
                                             const Components::MaterialRef& material, const OwnerLook& look);
    static bool RetireOwner(ECS::World& world, OwnerState& owner, Rendering::MeshGPURegistry* registry);
    bool RetireAll(ECS::World& world, Rendering::MeshGPURegistry* registry);

    SplineRebuildGate<ObservedState> m_Gate;
    ConformHold m_Hold;
    // The ground under all pavings' runs and the reach they read.
    ConformGroundWatch m_Ground;
    uint32 m_LoggedConformMisses = 0;
    // Key: the owning paving's entity id.
    FlatMap<uint32, OwnerState> m_Owners;
    PavingRoadTrims m_RoadTrims;
    std::vector<PavingApproach> m_Approaches;
    uint64 m_ApproachesVersion = 0;
    std::vector<uint32> m_HeldRoads;
    // Key: the paving's entity id.
    FlatMap<uint32, PavingStatus> m_Statuses;
    // Key: the paving's entity id; the notices last reported for it.
    FlatMap<uint32, std::vector<std::string>> m_LoggedNotices;
    // Key: the paving's entity id; its state at the last rebuild, kept while the gate waits.
    FlatMap<uint32, std::pair<BuildState, std::string_view>> m_LastStates;
    uint64 m_LastWorldResetGeneration = 0;
    bool m_DefaultMaterialRegistered = false;
    OverrideMaterialCache m_OverrideMaterials;
};

} // namespace GameEngine::SplineRecipes
