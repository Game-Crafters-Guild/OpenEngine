#pragma once

#include "AssetCore/GUID.h"
#include "Components/Spline/SplinePlacement.h"
#include "ECS/Entity.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector3.h"
#include "SplineLayout/TileLayout.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::Mathematics
{
struct Matrix4x4;
}
namespace GameEngine::SplineECS { class SplineService; }
namespace GameEngine::TerrainECS { class TerrainModifierSystem; }

namespace GameEngine::SplineRecipes
{

// The shared conform ray of the placement, fence and extrude controllers and the width-envelope gizmo.
// Each recipe conforms to its author's SplineConformTarget, but the gizmo always takes the whole scene,
// so over a prop the drawn centerline can sit on the prop while the pieces stand on the terrain.

// Every RuntimeOnlyEntity-tagged entity: the ignore list of the conform rays (PickOptions::IgnoreEntities),
// so recipe output, HLOD proxies and other engine-spawned entities are never their own ground.
[[nodiscard]] std::vector<ECS::EntityHandle> CollectConformRayIgnoreList(ECS::World& world);

// The conform-ray misses of one placement rebuild. A miss keeps the sample's authored altitude, so the piece
// hangs over ground the ray did not find; the rebuild reports the misses once, with a count and a witness.
struct ConformMissTally
{
    uint32 Probes = 0;
    uint32 Misses = 0;
    Mathematics::Vector3 FirstMiss{};
    // Rays that crossed a mesh whose BVH is still being built. They are not misses: the rebuild
    // holds (HoldForPendingBvh) and casts them again once the BVH is published.
    uint32 PendingBvh = 0;

    // The first missing sample is kept as the witness: it is the one a reader can
    // navigate to, and later misses in the same run are almost always its neighbours.
    void Record(bool hit, bool bvhPending, const Mathematics::Vector3& sample);
};

// Casts a ray down from above `sample` through Scene::RaycastScene; true, with `outHit` and `outNormal`, on a hit.
// The targets are the terrain and the renderers with a CPU mesh in every process; a pending BVH counts in `tally`.
// `target` is the author's ground: Scene takes the nearest surface, TerrainOnly ignores meshes and primitives.
bool ConformRayDown(ECS::World& world, const Mathematics::Vector3& sample,
                    std::span<const ECS::EntityHandle> ignore,
                    Mathematics::Vector3& outHit,
                    Mathematics::Vector3* outNormal = nullptr,
                    ConformMissTally* tally = nullptr,
                    Components::SplineConformTarget target =
                        Components::SplineConformTarget::Scene);

// A rebuild held on a mesh whose BVH is still being built: the controller keeps its previous output and its
// rebuild gate asks again on the next frame. The conform never waits for a BVH, because a wait stalls the frame.
struct ConformHold
{
    bool Held = false;     // the last rebuild held; the controller clears it before each rebuild
    bool Reported = false; // the hold was logged; cleared when a rebuild lands
};

// Ends a rebuild's conform once every ray is cast: true when a ray crossed a pending BVH, and then
// `hold` is marked held and the hold is logged once ("Held: BVH pending"); false lets the rebuild
// land and re-arms the log.
[[nodiscard]] bool HoldForPendingBvh(const ConformMissTally& tally, uint32 entityId,
                                     ConformHold& hold);

// Warns once for a rebuild whose conform rays missed: the count, the probe total, the witness and the ray's reach.
// `lastReportedMisses` is the caller's per-entity state, so a rebuild on each frame reports only a change.
void ReportConformMisses(const ConformMissTally& tally, uint32 entityId,
                         uint32& lastReportedMisses);

// A digest of the terrain ground the conform rays measure: the seven pick inputs (translation, SizeX, SizeZ,
// HeightScale, Domain) and the heightfield's content and identity. It does not cover mesh ground or a planet's drawn
// surface. Pass `modifierSystem` to read the composed ground of a tiled terrain, or null to conform by raycast.
[[nodiscard]] uint64 ConformSurfaceRevision(
    ECS::World& world, const TerrainECS::TerrainModifierSystem* modifierSystem);

// The surface term of one recipe's rebuild-settle observation: `worldDigest` when `readsSurface`, else 0, so terrain
// movement does not reset the settle clock of a recipe that does not read the ground. `readsSurface` is
// Components::ReadsSurface for the extrude and "conforms at all" for the placement and fence.
[[nodiscard]] uint64 ObservedSurfaceRevision(bool readsSurface, uint64 worldDigest);

// The term a Scene-conform recipe adds to its surface digest, and the scene TLAS version and
// run bounds it was read at.
struct SceneConformDigest
{
    static constexpr uint64 kNotRead = ~0ull;
    uint64 TlasVersion = kNotRead;
    Mathematics::AABB RunBounds = Mathematics::AABB::Empty();
    uint64 Value = 0u;
};

// Folds the scene's pickable meshes whose XZ bounds meet `runBounds` into `digest.Value` by entity, model, transform
// version and bounds; a runtime-only entity never counts. Walks the scene TLAS only when it is ready and its version
// or the bounds changed. Returns true when it walked.
bool RefreshSceneConformDigest(ECS::World& world, const Mathematics::AABB& runBounds,
                               SceneConformDigest& digest);

// Whether the ground of a conforming recipe exists yet. A ray miss is an authoring fact; a terrain that is not
// provisioned is a load-order fact that ends without a change by the author.
enum class ConformSurfaceReadiness : uint8
{
    // No terrain near the run: the recipe places against what is there.
    NoSurface,
    // A terrain is declared but neither of its data handles resolves yet. This is permanent only when
    // terrain extraction does not run, for example with no render device.
    Provisioning,
    // Every declared terrain has resolvable surface data.
    Ready
};

// The distance around a run inside which a conform target that is not loaded holds the run.
inline constexpr float32 kConformHoldMarginMetres = 50.0f;

// The readiness of the terrains near a run. A terrain counts when its footprint, widened by
// kConformHoldMarginMetres, meets the run's bounds in XZ. A terrain that is not loaded gives its
// footprint from its transform and its size, so a far terrain does not hold the run.
[[nodiscard]] ConformSurfaceReadiness ConformSurfaceState(ECS::World& world,
                                                          const Mathematics::AABB& runBounds);

// The world bounds of a spline's segments. Empty when the spline has no segment bounds.
[[nodiscard]] Mathematics::AABB RunWorldBounds(const SplineECS::SplineService& splineService,
                                               uint32 dataIndex, uint32 dataGeneration,
                                               const Mathematics::Matrix4x4& worldMatrix);

// The longest time, in seconds of delta time, that DeferFirstBuild holds a recipe's first build for its ground.
// About three times the 37 s wait measured on a 2859-entity island scene in DebugFast. The cause of that wait is not
// known, so this is an empirical bound. Under a fixed timestep, delta time is simulated time.
inline constexpr float32 kFirstBuildDeferBudgetSeconds = 120.0f;

// The longest time a chunk's first build waits for a model near it (ChunkModelHold), in seconds
// of delta time. Twice the slowest measured cold-cache scene load (32.4 s for 20,804 meshes).
inline constexpr float32 kConformHoldBudgetSeconds = 65.0f;

// A MeshRenderer whose model is not loaded: a conform target that can still land.
struct PendingConformTarget
{
    Mathematics::Vector3 Position; // the entity's WorldTransform translation
    GUID Model;
    bool LoadFailed = false; // the asset manager refuses to load the model again
};

// The MeshRenderers whose models are not loaded. A runtime-only entity, a primitive and a null
// model never count. Empty when the engine has no asset manager.
[[nodiscard]] std::vector<PendingConformTarget> CollectPendingConformTargets(ECS::World& world);

// The chunks of one Scene-conform run whose first build waits for a model near them. A chunk
// waits while a pending target lies inside its station bounds in XZ, widened by
// kConformHoldMarginMetres. A failed load or a spent budget ends the wait.
struct ChunkModelHold
{
    std::vector<bool> Landed;                 // per chunk slot: built once with no wait
    std::vector<Mathematics::AABB> Bounds; // the widened bounds of each waiting chunk
    uint32 WaitingModels = 0;
    float32 HeldSeconds = 0.0f;
    std::string HeldReason;   // set while a chunk waits
    std::string FailedReason; // names the model when a wait ended without it
};

// Marks the chunks of `chunkRanges` that wait, for a build whose draped centerline is `center`
// (world space, one sample per station). Returns one flag per chunk, true for a chunk that waits: the caller builds it empty.
[[nodiscard]] std::vector<bool> HoldChunksForPendingModels(
    std::span<const SplineLayout::CenterSample> center,
    std::span<const std::pair<uint32, uint32>> chunkRanges,
    std::span<const PendingConformTarget> targets, uint32 entityId, ChunkModelHold& hold);

// Whether a waiting chunk can build now: no pending target is left inside its bounds, a target's
// load failed, or the budget is spent.
[[nodiscard]] bool ChunkModelHoldReleases(const ChunkModelHold& hold,
                                          std::span<const PendingConformTarget> targets);

// Whether a recipe's first build waits this frame because its ground is not provisioned or not settled
// (`surfaceSettled` is the caller's verdict). Both waits share kFirstBuildDeferBudgetSeconds in `deferredSeconds`:
// when the budget is spent, the function warns once through `warned` and the build runs.
[[nodiscard]] bool DeferFirstBuild(ConformSurfaceReadiness readiness, bool readsSurface,
                                   bool appliedOnce, bool surfaceSettled, float32 deltaSeconds,
                                   uint32 entityId, float32& deferredSeconds, bool& warned);

// Warns once for a rebuild whose ground leaned past the recipe's tilt ceiling: the held stations and the steepest
// slope. The pieces still place, but held tiles cut into the face. Same dedupe contract as ReportConformMisses.
void ReportTiltClamp(const SplineLayout::TiltClampReport& report, uint32 entityId, float32 maxTiltDegrees,
                     uint32& lastReportedClamps);

// Warns once for a rebuild whose spans stood too near vertical to roll and took their first station's frame, so the
// run looks broken. Same dedupe contract as ReportConformMisses; silent when no span borrowed.
void ReportSpanRollBorrowed(uint32 borrowedSpans, uint32 entityId, uint32& lastReportedBorrows);

} // namespace GameEngine::SplineRecipes
