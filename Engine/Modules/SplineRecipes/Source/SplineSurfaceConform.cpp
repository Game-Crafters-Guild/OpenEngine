#include "SplineRecipes/SplineSurfaceConform.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Ray.h"
#include "Scene/SceneRaycast.h"
#include "Scene/SceneTlas.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "Types/Fnv1a.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace GameEngine::SplineRecipes
{
namespace
{

// One mesh's term of the scene conform digest: 0 when its bounds miss `runBounds` in XZ or it is
// runtime-only, else a hash of its entity, model, transform version and bounds.
uint64 SceneConformLeafHash(ECS::World& world, const Mathematics::AABB& runBounds, const Scene::TlasInstance& leaf)
{
    const Mathematics::AABB& bounds = leaf.WorldBounds;
    if (bounds.max.x < runBounds.min.x || bounds.min.x > runBounds.max.x || bounds.max.z < runBounds.min.z ||
        bounds.min.z > runBounds.max.z || world.HasComponent<Components::RuntimeOnlyEntity>(leaf.Entity))
        return 0u;
    const auto* renderer = world.GetComponent<Components::MeshRenderer>(leaf.Entity);
    const auto* transform = world.GetComponent<Components::WorldTransform>(leaf.Entity);
    uint8 model[GUID::kSize] = {};
    if (renderer)
        renderer->modelAssetGuid.ToGuid().WriteBytes(model);
    const uint32 version = transform ? transform->Version : 0u;
    uint64 hash = Hashing::Fnv1a64(&leaf.Entity.id, sizeof(leaf.Entity.id));
    hash = Hashing::Fnv1a64(model, sizeof(model), hash);
    hash = Hashing::Fnv1a64(&version, sizeof(version), hash);
    return Hashing::Fnv1a64(&bounds, sizeof(bounds), hash);
}

// Conform rays start this far above the sample and probe straight down.
constexpr float32 kConformRayLift = 100.0f;
constexpr float32 kConformRayMaxDistance = 1000.0f;

// Order-sensitive 64-bit fold. A change detector needs only "a different input
// gives a different digest"; the query order that feeds it is stable within a
// frame, which is all the ordering this relies on.
void FoldRevision(uint64& digest, uint64 value)
{
    digest ^= value + 0x9E3779B97F4A7C15ull + (digest << 6) + (digest >> 2);
}

// A float's bits, so the fold detects a change: NaN equals itself here and +0 and -0 differ.
// TerrainModifierSystem's terrain-state hash reads the same fields.
uint64 FloatBits(float32 value)
{
    uint32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint64>(bits);
}

// Whether a terrain's footprint, widened by kConformHoldMarginMetres, meets `run` in XZ. The
// footprint comes from the transform and the size, so a terrain that is not loaded has one. A planet
// is centered on the world origin and reaches HeightScale above PlanetRadius.
bool TerrainNearRun(const Components::Terrain& terrain, const Components::WorldTransform& worldXf,
                    const Mathematics::AABB& run)
{
    if (terrain.Domain == Components::TerrainDomain::Spherical)
    {
        const float32 nearestX = std::clamp(0.0f, run.min.x, run.max.x);
        const float32 nearestY = std::clamp(0.0f, run.min.y, run.max.y);
        const float32 nearestZ = std::clamp(0.0f, run.min.z, run.max.z);
        const float32 reach = terrain.PlanetRadius + std::abs(terrain.HeightScale) + kConformHoldMarginMetres;
        return nearestX * nearestX + nearestY * nearestY + nearestZ * nearestZ <= reach * reach;
    }
    const float32 halfX = terrain.SizeX * 0.5f + kConformHoldMarginMetres;
    const float32 halfZ = terrain.SizeZ * 0.5f + kConformHoldMarginMetres;
    const Mathematics::Vector3 center = worldXf.GetPosition();
    const float32 centerX = center.x;
    const float32 centerZ = center.z;
    return centerX - halfX <= run.max.x && centerX + halfX >= run.min.x && centerZ - halfZ <= run.max.z &&
           centerZ + halfZ >= run.min.z;
}

// Folds every input TerrainPicking turns into a world-space hit, besides the samples: the
// translation, the size, the height scale and the domain, as bit patterns. A terrain without
// modifiers never bakes, so its heightfield version stays the same when it moves.
void FoldTerrainRevision(uint64& revision, TerrainECS::TerrainService& terrainService, uint64 appliedGroundRevision,
                         ECS::EntityHandle entity, const Components::Terrain& terrain,
                         const Components::WorldTransform& worldXf)
{
    const Mathematics::Vector3 position = worldXf.GetPosition();
    FoldRevision(revision, FloatBits(position.x));
    FoldRevision(revision, FloatBits(position.y));
    FoldRevision(revision, FloatBits(position.z));
    FoldRevision(revision, FloatBits(terrain.SizeX));
    FoldRevision(revision, FloatBits(terrain.SizeZ));
    FoldRevision(revision, FloatBits(terrain.HeightScale));
    FoldRevision(revision, static_cast<uint64>(terrain.Domain));

    // The identity too: a terrain made again starts its version at 0.
    FoldRevision(revision, entity.id);
    FoldRevision(revision, (static_cast<uint64>(terrain.TerrainDataGeneration) << 32) | terrain.TerrainDataHandle);
    FoldRevision(revision, (static_cast<uint64>(terrain.TiledTerrainGeneration) << 32) | terrain.TiledTerrainHandle);

    if (const auto* data = terrainService.GetTerrainData(
            TerrainECS::TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration}))
        FoldRevision(revision, data->HeightfieldVersion);

    const auto* tiled = terrainService.GetTiledTerrainData(
        TerrainECS::TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration});
    if (!tiled)
        return;
    // The tile versions cover only the resident tiles. The applied ground revision covers an
    // edit away from the camera, which changes the composed ground and no resident tile.
    FoldRevision(revision, appliedGroundRevision);
    for (const auto& entry : tiled->Tiles)
    {
        const auto& tile = entry.second;
        if (!tile || tile->LodState == TerrainECS::TileLodState::Empty)
            continue;
        // The coordinate goes with the version, so an evicted tile and an arrived tile at the
        // same version do not cancel.
        FoldRevision(revision, (static_cast<uint64>(static_cast<uint32>(entry.first.X)) << 32) |
                                   static_cast<uint32>(entry.first.Z));
        FoldRevision(revision, tile->HeightfieldVersion);
    }
}

// A planar terrain has its data handle and a tiled terrain the other one. A terrain with
// neither is declared but not loaded yet.
bool TerrainProvisioned(TerrainECS::TerrainService& terrainService, const Components::Terrain& terrain)
{
    return terrainService.GetTerrainData(
               TerrainECS::TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration}) != nullptr ||
           terrainService.GetTiledTerrainData(
               TerrainECS::TiledTerrainHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration}) != nullptr;
}

// Adds the entity when its model is not loaded. A runtime-only entity, a primitive and a null
// model never count.
void AddPendingConformTarget(ECS::World& world, const AssetManager& assets, ECS::EntityHandle entity,
                             const Components::MeshRenderer& renderer, const Components::WorldTransform& transform,
                             std::vector<PendingConformTarget>& targets)
{
    const GUID model = renderer.modelAssetGuid.ToGuid();
    if (model.IsNull() || Engine::Renderer::PrimitiveGenerator::IsPrimitive(model) || assets.IsAssetLoaded(model) ||
        world.HasComponent<Components::RuntimeOnlyEntity>(entity))
        return;
    targets.push_back({transform.GetPosition(), model, assets.IsLoadSuppressed(model)});
}

} // namespace

std::vector<ECS::EntityHandle> CollectConformRayIgnoreList(ECS::World& world)
{
    std::vector<ECS::EntityHandle> ignore;
    // Generated geometry is cleaned up whatever its enable state.
    world.Query<ECS::Read<Components::RuntimeOnlyEntity>>()
        .IncludeDisabled()
        .Each([&](ECS::EntityHandle e, const Components::RuntimeOnlyEntity&)
        {
            ignore.push_back(e);
        });
    return ignore;
}

void ConformMissTally::Record(bool hit, bool bvhPending, const Mathematics::Vector3& sample)
{
    ++Probes;
    if (bvhPending)
    {
        ++PendingBvh;
        return;
    }
    if (hit)
        return;
    if (Misses == 0u)
        FirstMiss = sample;
    ++Misses;
}

bool ConformRayDown(ECS::World& world, const Mathematics::Vector3& sample,
                    std::span<const ECS::EntityHandle> ignore,
                    Mathematics::Vector3& outHit,
                    Mathematics::Vector3* outNormal,
                    ConformMissTally* tally,
                    Components::SplineConformTarget target)
{
    Mathematics::Ray3D ray;
    ray.origin = Mathematics::Vector3(sample.x, sample.y + kConformRayLift, sample.z);
    ray.direction = Mathematics::Vector3(0.0f, -1.0f, 0.0f);

    Scene::PickOptions options;
    options.MaxDistance = kConformRayMaxDistance;
    options.IgnoreEntities = ignore;
    // The bounds fallback stays off (the default): a renderer's box is not ground. A mesh still
    // preparing its picking data gives no hit and marks the result pending, which holds the
    // rebuild (HoldForPendingBvh).
    if (target == Components::SplineConformTarget::TerrainOnly)
    {
        options.IncludeMeshes = false;
        options.IncludePrimitives = false;
    }
    const Scene::PickResult result = Scene::RaycastScene(ray, world, options);
    if (tally)
        tally->Record(result.Hit, result.MeshBvhPending, sample);
    if (!result.Hit)
        return false;
    outHit = result.Best.WorldPosition;
    if (outNormal)
        *outNormal = result.Best.WorldNormal;
    return true;
}

void ReportConformMisses(const ConformMissTally& tally, uint32 entityId,
                         uint32& lastReportedMisses)
{
    if (tally.Misses == lastReportedMisses)
        return;
    lastReportedMisses = tally.Misses;
    if (tally.Misses == 0u)
        return;

    Logger::Log::Warning(
        "SplineConform: entity {} found no ground under {} of {} samples — those keep their "
        "authored altitude, so pieces there float clear of the surface. First at "
        "({:.2f}, {:.2f}, {:.2f}). Each ray starts {:.0f} m above its sample and probes {:.0f} m "
        "down: check the spline lies inside the terrain footprint and that the ground under it "
        "is within that reach.",
        entityId, tally.Misses, tally.Probes,
        tally.FirstMiss.x, tally.FirstMiss.y, tally.FirstMiss.z,
        kConformRayLift, kConformRayMaxDistance);
}

bool HoldForPendingBvh(const ConformMissTally& tally, uint32 entityId, ConformHold& hold)
{
    hold.Held = tally.PendingBvh > 0u;
    if (!hold.Held)
    {
        hold.Reported = false;
        return false;
    }
    if (!hold.Reported)
    {
        hold.Reported = true;
        Logger::Log::Info(
            "SplineConform: entity {} held: waiting for the picking data of a mesh under {} of {} "
            "samples. The rebuild keeps the previous pieces and lands when the data is ready.",
            entityId, tally.PendingBvh, tally.Probes);
    }
    return true;
}

uint64 ConformSurfaceRevision(ECS::World& world,
                              const TerrainECS::TerrainModifierSystem* modifierSystem)
{
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
        return 0u;

    const uint64 appliedGroundRevision =
        modifierSystem ? modifierSystem->GetAppliedGroundRevision() : 0u;

    uint64 revision = 0u;
    FoldRevision(revision, terrainService->SphereSculptVersion());

    // The same query TerrainPicking marches, so the digest covers exactly the
    // terrains a conform ray can hit: a terrain the pick ignores must not be
    // able to schedule a rebuild.
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& worldXf)
        { FoldTerrainRevision(revision, *terrainService, appliedGroundRevision, entity, terrain, worldXf); });

    return revision;
}

uint64 ObservedSurfaceRevision(bool readsSurface, uint64 worldDigest)
{
    return readsSurface ? worldDigest : 0u;
}

ConformSurfaceReadiness ConformSurfaceState(ECS::World& world, const Mathematics::AABB& runBounds)
{
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService || runBounds.IsEmpty())
        return ConformSurfaceReadiness::NoSurface;

    bool anyTerrain = false;
    bool anyUnprovisioned = false;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](const Components::Terrain& terrain, const Components::WorldTransform& worldXf)
        {
            if (!TerrainNearRun(terrain, worldXf, runBounds))
                return;
            anyTerrain = true;
            anyUnprovisioned = anyUnprovisioned || !TerrainProvisioned(*terrainService, terrain);
        });

    if (!anyTerrain)
        return ConformSurfaceReadiness::NoSurface;
    return anyUnprovisioned ? ConformSurfaceReadiness::Provisioning
                            : ConformSurfaceReadiness::Ready;
}

Mathematics::AABB RunWorldBounds(const SplineECS::SplineService& splineService, uint32 dataIndex,
                                 uint32 dataGeneration, const Mathematics::Matrix4x4& worldMatrix)
{
    Mathematics::AABB bounds = Mathematics::AABB::Empty();
    const Spline::SplineData* data =
        splineService.GetSplineData(SplineECS::SplineHandle(dataIndex, dataGeneration));
    if (!data)
        return bounds;
    for (const Mathematics::AABB& segment : data->SegmentBounds)
        bounds.Expand(Mathematics::BoundingBox::FromMinMax(segment.min, segment.max).TransformToAABB(worldMatrix.Data()));
    return bounds;
}

bool RefreshSceneConformDigest(ECS::World& world, const Mathematics::AABB& runBounds,
                               SceneConformDigest& digest)
{
    Scene::SceneTlas& tlas = Scene::GetSceneTlas(world);
    if (tlas.RequestCurrent(world) != Scene::SceneTlas::Readiness::Ready)
        return false;
    const uint64 tlasVersion = tlas.Version();
    if (tlasVersion == digest.TlasVersion && std::memcmp(&runBounds, &digest.RunBounds, sizeof(runBounds)) == 0)
        return false;
    digest.TlasVersion = tlasVersion;
    digest.RunBounds = runBounds;
    digest.Value = 0u;
    if (runBounds.IsEmpty())
        return true;
    const Mathematics::Vector3 center = (runBounds.min + runBounds.max) * 0.5f;
    const float32 radius = (runBounds.max - center).Length();
    // A sum of per-mesh hashes, so the traversal order does not change it.
    uint64& value = digest.Value;
    tlas.TraverseSphere(center, radius, {}, [&world, &runBounds, &value](const Scene::TlasInstance& leaf) {
        value += SceneConformLeafHash(world, runBounds, leaf);
    });
    return true;
}

std::vector<PendingConformTarget> CollectPendingConformTargets(ECS::World& world)
{
    std::vector<PendingConformTarget> targets;
    const AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return targets;
    world.Query<ECS::Read<Components::MeshRenderer>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle e, const Components::MeshRenderer& renderer, const Components::WorldTransform& xf)
        { AddPendingConformTarget(world, *assets, e, renderer, xf, targets); });
    return targets;
}

namespace
{

bool InsideXZ(const Mathematics::Vector3& point, const Mathematics::AABB& bounds)
{
    return point.x >= bounds.min.x && point.x <= bounds.max.x && point.z >= bounds.min.z && point.z <= bounds.max.z;
}

} // namespace

std::vector<bool> HoldChunksForPendingModels(std::span<const SplineLayout::CenterSample> center,
                                             std::span<const std::pair<uint32, uint32>> chunkRanges,
                                             std::span<const PendingConformTarget> targets, uint32 entityId,
                                             ChunkModelHold& hold)
{
    std::vector<bool> waits(chunkRanges.size(), false);
    hold.Landed.resize(chunkRanges.size(), false);
    hold.Bounds.clear();
    hold.WaitingModels = 0;
    const bool budgetSpent = hold.HeldSeconds >= kConformHoldBudgetSeconds;
    GUID missedModel;
    bool missedOnFailure = false;
    for (size_t c = 0; c < chunkRanges.size(); ++c)
    {
        if (hold.Landed[c])
            continue;
        const auto [first, last] = chunkRanges[c];
        Mathematics::AABB bounds = Mathematics::AABB::Empty();
        for (uint32 i = first; i <= last; ++i)
            bounds.Expand(center[i].Pos);
        bounds.Inflate(Mathematics::Vector3(kConformHoldMarginMetres, 0.0f, kConformHoldMarginMetres));
        for (const PendingConformTarget& target : targets)
        {
            if (!InsideXZ(target.Position, bounds))
                continue;
            if (target.LoadFailed || budgetSpent)
            {
                if (missedModel.IsNull())
                {
                    missedModel = target.Model;
                    missedOnFailure = target.LoadFailed;
                }
                continue;
            }
            waits[c] = true;
            ++hold.WaitingModels;
        }
        if (waits[c])
            hold.Bounds.push_back(bounds);
        else
        {
            hold.Landed[c] = true;
        }
    }

    hold.HeldReason.clear();
    if (hold.WaitingModels > 0u)
        hold.HeldReason = hold.WaitingModels == 1u
                              ? std::string("1 model near it is still loading. It builds when the model loads.")
                              : std::format("{} models near it are still loading. It builds when they load.",
                                            hold.WaitingModels);
    else
        hold.HeldSeconds = 0.0f;

    hold.FailedReason.clear();
    if (!missedModel.IsNull())
    {
        hold.FailedReason = std::format("A model near it did not {} ({}). It built without the model.",
                                        missedOnFailure ? "load" : "load in time", missedModel.ToString());
        Logger::Log::Warning("Spline recipe: entity {}: {}", entityId, hold.FailedReason);
    }
    return waits;
}

bool ChunkModelHoldReleases(const ChunkModelHold& hold, std::span<const PendingConformTarget> targets)
{
    if (hold.HeldSeconds >= kConformHoldBudgetSeconds)
        return true;
    for (const Mathematics::AABB& bounds : hold.Bounds)
    {
        bool waits = false;
        for (const PendingConformTarget& target : targets)
        {
            if (!InsideXZ(target.Position, bounds))
                continue;
            if (target.LoadFailed)
                return true;
            waits = true;
        }
        if (!waits)
            return true;
    }
    return false;
}

bool DeferFirstBuild(ConformSurfaceReadiness readiness, bool readsSurface, bool appliedOnce,
                     bool surfaceSettled, float32 deltaSeconds, uint32 entityId,
                     float32& deferredSeconds, bool& warned)
{
    // Nothing to wait for: already built once, reads no ground, or the scene
    // carries no terrain at all and none is coming.
    if (appliedOnce || !readsSurface || readiness == ConformSurfaceReadiness::NoSurface)
    {
        deferredSeconds = 0.0f;
        return false;
    }

    // The ground is here AND has stopped moving — the only state a first
    // measurement can be trusted against.
    if (readiness == ConformSurfaceReadiness::Ready && surfaceSettled)
    {
        deferredSeconds = 0.0f;
        return false;
    }

    deferredSeconds += deltaSeconds;
    if (deferredSeconds < kFirstBuildDeferBudgetSeconds)
        return true;

    if (!warned)
    {
        warned = true;
        Logger::Log::Warning(
            "SplineConform: entity {} waited {:.0f} s for ground that never became final — the "
            "terrain never provisioned its surface data, or it has not stopped changing — so it "
            "is placing against whatever lies under it now. Pieces there may stand on props, or "
            "hold their authored altitude where nothing does. Check that the terrain entity's "
            "asset reference resolves and that its tiles have finished streaming.",
            entityId, deferredSeconds);
    }
    return false;
}

void ReportTiltClamp(const SplineLayout::TiltClampReport& report, uint32 entityId, float32 maxTiltDegrees,
                     uint32& lastReportedClamps)
{
    if (report.ClampedStations == lastReportedClamps)
        return;
    lastReportedClamps = report.ClampedStations;
    if (report.ClampedStations == 0u)
        return;

    Logger::Log::Warning(
        "SplinePlacement: entity {} has {} station(s) on ground steeper than the {:.0f} degree "
        "placement tilt ceiling — steepest {:.0f} degrees at ({:.2f}, {:.2f}, {:.2f}). Those "
        "pieces still conform in height but hold the ceiling, so they cut into the face instead "
        "of lying on it. Raise Max Tilt to follow the ground further, or route the spline around "
        "the steep section.",
        entityId, report.ClampedStations, maxTiltDegrees, report.SteepestDegrees,
        report.SteepestAt.x, report.SteepestAt.y, report.SteepestAt.z);
}

void ReportSpanRollBorrowed(uint32 borrowedSpans, uint32 entityId, uint32& lastReportedBorrows)
{
    if (borrowedSpans == lastReportedBorrows)
        return;
    lastReportedBorrows = borrowedSpans;
    if (borrowedSpans == 0u)
        return;

    Logger::Log::Warning(
        "SplineFence: entity {} has {} span(s) whose two posts stand almost vertically above one "
        "another — ground that steep leaves a span no roll of its own, so those spans hold their "
        "opening post's frame and stop reaching both posts. Add an authored point at the top and "
        "the bottom of the face, or route the run across it rather than straight up it.",
        entityId, borrowedSpans);
}

} // namespace GameEngine::SplineRecipes
