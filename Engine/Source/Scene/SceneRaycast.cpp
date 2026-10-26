#include "Scene/SceneRaycast.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector4.h"
#include "MeshPicking/MeshBvh.h"
#include "MeshPicking/PickTypes.h"
#include "Scene/MeshBvhCache.h"
#include "Scene/SceneRaycastHost.h"
#include "Scene/SceneTlas.h"
#include "Scene/TerrainPicking.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace GameEngine::Scene
{

namespace
{

using Mathematics::Vector3;

// Produce the renderer's model GUID and a validity flag (some renderers
// haven't been assigned a model yet).
bool ExtractGuid(const Components::MeshRenderer& mr, GUID& outGuid)
{
    if (mr.modelAssetGuid.IsNull())
        return false;
    outGuid = mr.modelAssetGuid.ToGuid();
    return true;
}

// Resolve the CPU mesh data for a renderer. For built-in primitives the
// geometry comes from PrimitiveGenerator::CachedMeshByGuid; for asset-backed
// renderers we fetch the ModelAsset out of AssetManager (no force-load:
// silently skip if not yet resident). Returns nullptr on miss.
const Mesh* ResolveCpuMesh(const GUID& guid,
                           uint32 submesh,
                           PickableKind& outKind)
{
    if (const Mesh* prim = Engine::Renderer::PrimitiveGenerator::CachedMeshByGuid(guid))
    {
        outKind = PickableKind::Primitive;
        return prim;
    }

    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    auto asset = assetManager.GetAsset(guid);
    if (!asset)
        return nullptr;
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return nullptr;
    if (submesh >= modelAsset->GetMeshCount())
        return nullptr;

    outKind = PickableKind::Mesh;
    return &modelAsset->GetMesh(submesh);
}

const Mesh* ResolveRegisteredCpuMesh(const Components::MeshRenderer& mr,
                                     const GUID* guid,
                                     PickableKind& outKind)
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!renderServices)
        return nullptr;

    const Rendering::MeshGPURegistry& registry = renderServices->GetMeshGPURegistry();
    const Rendering::MeshGPUEntry* entry = nullptr;

    if (mr.meshGpuHandleId != 0u)
    {
        entry = registry.Find(Rendering::MeshGPUHandle(mr.meshGpuHandleId));
    }
    if (!entry && guid && !guid->IsNull())
    {
        entry = registry.FindByKey(Rendering::MeshGPUKey{*guid, mr.meshId});
    }

    if (!entry || !entry->cpuMesh)
        return nullptr;

    outKind = PickableKind::Mesh;
    return entry->cpuMesh.get();
}

// World-space triangle normal at the hit: the local face normal through the inverse transpose of
// the world matrix's upper 3x3, renormalized, so it stays perpendicular to the face under
// non-uniform scale. `inverseWorld` is the inverse of the entity's world matrix.
Vector3 ComputeWorldNormal(const Mesh& mesh,
                           uint32 triangleIndex,
                           const Mathematics::Matrix4x4& inverseWorld)
{
    if (triangleIndex == ~0u)
        return Vector3(0.0f, 1.0f, 0.0f);
    const uint32 base = triangleIndex * 3u;
    if (base + 2u >= mesh.Indices.size())
        return Vector3(0.0f, 1.0f, 0.0f);
    const uint32 i0 = mesh.Indices[base + 0];
    const uint32 i1 = mesh.Indices[base + 1];
    const uint32 i2 = mesh.Indices[base + 2];
    if (i0 >= mesh.Vertices.size() || i1 >= mesh.Vertices.size() || i2 >= mesh.Vertices.size())
        return Vector3(0.0f, 1.0f, 0.0f);

    const auto& a = mesh.Vertices[i0].Position;
    const auto& b = mesh.Vertices[i1].Position;
    const auto& c = mesh.Vertices[i2].Position;
    const Vector3 v0(a[0], a[1], a[2]);
    const Vector3 v1(b[0], b[1], b[2]);
    const Vector3 v2(c[0], c[1], c[2]);
    const Vector3 nLocal = Vector3::Cross(v1 - v0, v2 - v0).Normalize();

    // w = 0 keeps the translation out; only the upper 3x3 of the transpose reaches xyz.
    const Mathematics::Vector4 transformed = Mathematics::Transpose(inverseWorld).Transform(
        Mathematics::Vector4(nLocal.x, nLocal.y, nLocal.z, 0.0f));
    const Vector3 worldNormal(transformed.x, transformed.y, transformed.z);
    const float32 len = worldNormal.Length();
    if (len > 1e-6f)
        return worldNormal * (1.0f / len);
    return Vector3(0.0f, 1.0f, 0.0f);
}

Vector3 ComputeAabbHitNormal(const Mathematics::AABB& box, const Vector3& point)
{
    struct Candidate
    {
        float32 Distance;
        Vector3 Normal;
    };

    const Candidate candidates[] = {
        {std::fabs(point.x - box.min.x), Vector3(-1.0f, 0.0f, 0.0f)},
        {std::fabs(point.x - box.max.x), Vector3( 1.0f, 0.0f, 0.0f)},
        {std::fabs(point.y - box.min.y), Vector3(0.0f, -1.0f, 0.0f)},
        {std::fabs(point.y - box.max.y), Vector3(0.0f,  1.0f, 0.0f)},
        {std::fabs(point.z - box.min.z), Vector3(0.0f, 0.0f, -1.0f)},
        {std::fabs(point.z - box.max.z), Vector3(0.0f, 0.0f,  1.0f)},
    };

    const Candidate* best = &candidates[0];
    for (const Candidate& candidate : candidates)
    {
        if (candidate.Distance < best->Distance)
            best = &candidate;
    }
    return best->Normal;
}

bool FillBoundsFallbackHit(const Mathematics::Ray3D& ray,
                           GameEngine::ECS::EntityHandle entity,
                           const Mathematics::AABB& box,
                           float32 boxEntry,
                           PickHit& outHit)
{
    if (boxEntry < 0.0f)
        boxEntry = 0.0f;

    const Vector3 worldHit(ray.origin.x + ray.direction.x * boxEntry,
                           ray.origin.y + ray.direction.y * boxEntry,
                           ray.origin.z + ray.direction.z * boxEntry);

    outHit.Kind          = PickableKind::Bounds;
    outHit.Entity        = entity;
    outHit.SubmeshIndex  = 0u;
    outHit.TriangleIndex = ~0u;
    outHit.Barycentrics  = Vector3{};
    outHit.WorldPosition = worldHit;
    outHit.WorldNormal   = ComputeAabbHitNormal(box, worldHit);
    outHit.Distance      = boxEntry;
    return true;
}

// Inner narrow-phase test for a single entity. AABB cull, fetch CPU mesh,
// transform the ray to local space, run BVH or brute-force per-triangle.
// Returns true and fills outHit on hit. maxT bounds the ray length.
bool TestEntityHit(const Mathematics::Ray3D& ray,
                   GameEngine::ECS::EntityHandle entity,
                   const Components::WorldTransform& worldXf,
                   const Components::MeshRenderer& mr,
                   const Components::LocalBounds* bounds,
                   const PickOptions& options,
                   float32 maxT,
                   PickHit& outHit)
{
    if (!mr.RenderLayers.Intersects(options.LayerMask))
        return false;
    // A shadows-only mesh draws in no color pass: a ray passes through it, and the user selects it
    // in the Hierarchy.
    if (mr.shadowCasting == Components::ShadowCasting::ShadowsOnly)
        return false;

    const Mathematics::BoundingBox kDefault{};
    const auto box = (bounds ? bounds->Box : kDefault).TransformToAABB(worldXf.matrix);
    float32 boxTMin = 0.0f, boxTMax = 0.0f;
    if (!Mathematics::IntersectRayAABB(ray, box, boxTMin, boxTMax))
        return false;
    const float32 boxEntry = (boxTMin >= 0.0f) ? boxTMin : 0.0f;
    if (boxEntry >= maxT)
        return false;

    PickableKind kind = PickableKind::Mesh;
    const Mesh* meshPtr = nullptr;
    GUID guid{};
    if (ExtractGuid(mr, guid))
        meshPtr = ResolveCpuMesh(guid, mr.meshId, kind);
    if (!meshPtr)
        meshPtr = ResolveRegisteredCpuMesh(mr, guid.IsNull() ? nullptr : &guid, kind);
    if (!meshPtr || meshPtr->Indices.empty() || meshPtr->Vertices.empty())
    {
        return options.IncludeBoundsFallback
            ? FillBoundsFallbackHit(ray, entity, box, boxEntry, outHit)
            : false;
    }
    if (kind == PickableKind::Primitive && !options.IncludePrimitives)
        return false;
    if (kind == PickableKind::Mesh && !options.IncludeMeshes)
        return false;

    const Mathematics::Matrix4x4 worldM = Mathematics::Matrix4x4::FromColumnMajor(worldXf.matrix);
    const Mathematics::Matrix4x4 invM = Mathematics::Inverse(worldM);

    const Vector3 worldOrigin(ray.origin.x, ray.origin.y, ray.origin.z);
    const Vector3 worldDir   (ray.direction.x, ray.direction.y, ray.direction.z);
    const Vector3 localOrigin = invM.TransformPoint(worldOrigin);
    const Vector3 localDirEnd = invM.TransformPoint(worldOrigin + worldDir);
    const Vector3 localDir    = localDirEnd - localOrigin;

    MeshPicking::PickHit raw;
    bool hit = false;
    bool testedByBvh = false;
    const uint32 triCount = static_cast<uint32>(meshPtr->Indices.size() / 3u);
    if (triCount >= MeshPicking::kMeshAutoBvhThreshold)
    {
        const MeshBvhLookup lookup = MeshBvhCache::Instance().TryGet(guid, mr.meshId, *meshPtr);
        // Testing every triangle instead would cost the calling thread what the job saves it.
        if (lookup.Pending)
        {
            if (!options.IncludeBoundsFallback)
            {
                // No hit, and the caller learns the mesh may change its answer.
                outHit.MeshBvhPending = true;
                outHit.Distance = boxEntry;
                return false;
            }
            FillBoundsFallbackHit(ray, entity, box, boxEntry, outHit);
            outHit.MeshBvhPending = true;
            return true;
        }
        if (lookup.Bvh)
        {
            hit = lookup.Bvh->Raycast(localOrigin, localDir, raw);
            testedByBvh = true;
        }
    }
    // Every triangle is tested only for a mesh without a BVH: one below kMeshAutoBvhThreshold,
    // or one whose BVH came out empty or failed to build. A BVH's miss is final.
    if (!testedByBvh)
    {
        MeshPicking::MeshView view;
        view.Positions    = reinterpret_cast<const Vector3*>(&meshPtr->Vertices[0].Position[0]);
        view.VertexStride = sizeof(Vertex);
        view.VertexCount  = static_cast<uint32>(meshPtr->Vertices.size());
        view.IndexCount   = static_cast<uint32>(meshPtr->Indices.size());
        view.Indices      = meshPtr->Indices.data();
        hit = MeshPicking::RayMesh(view, localOrigin, localDir, raw);
    }
    if (!hit || raw.Distance >= maxT)
        return false;

    const Vector3 worldHit(ray.origin.x + ray.direction.x * raw.Distance,
                           ray.origin.y + ray.direction.y * raw.Distance,
                           ray.origin.z + ray.direction.z * raw.Distance);

    outHit.Kind          = kind;
    outHit.Entity        = entity;
    outHit.SubmeshIndex  = mr.meshId;
    outHit.TriangleIndex = raw.TriangleIndex;
    outHit.Barycentrics  = raw.Barycentrics;
    outHit.WorldPosition = worldHit;
    outHit.WorldNormal   = ComputeWorldNormal(*meshPtr, raw.TriangleIndex, invM);
    outHit.Distance      = raw.Distance;
    return true;
}

bool IsSkipped(GameEngine::ECS::World& world, GameEngine::ECS::EntityHandle entity,
               const HostRaycastInputs& host)
{
    return host.SkipEntity && host.SkipEntity(world, entity);
}

bool RaycastSingleEntity(const Mathematics::Ray3D& ray,
                         GameEngine::ECS::EntityHandle entity,
                         GameEngine::ECS::World& world,
                         float32 maxT,
                         const PickOptions& options,
                         const HostRaycastInputs& host,
                         PickHit& outHit)
{
    if (!world.IsValid(entity))
        return false;
    const auto* worldXf = world.GetComponent<Components::WorldTransform>(entity);
    const auto* mr      = world.GetComponent<Components::MeshRenderer>(entity);
    const auto* bounds  = world.GetComponent<Components::LocalBounds>(entity);
    const auto* sector  = world.GetComponent<Components::WorldSectorCoord>(entity);
    if (!worldXf || !mr || IsSkipped(world, entity, host))
        return false;
    const float32 sectorSize = GetSceneTlas(world).GetSectorSize();
    const Components::WorldTransform effectiveXf =
        Components::ComposeEffectiveWorldTransform(*worldXf, sector, sectorSize);
    return TestEntityHit(ray, entity, effectiveXf, *mr, bounds, options, maxT, outHit);
}

bool IsIgnored(ECS::EntityHandle entity, const PickOptions& options)
{
    for (ECS::EntityHandle ignored : options.IgnoreEntities)
    {
        if (ignored == entity)
            return true;
    }
    return false;
}

// Raycast one entity through a mesh handed over as data (HostRaycastInputs::ExtraMeshes): the same
// narrow phase as a renderer's, with the sector compose and the layer and ignore rules.
bool RaycastExtraMesh(const Mathematics::Ray3D& ray,
                      ECS::World& world,
                      float32 maxT,
                      const PickOptions& options,
                      const RaycastMesh& mesh,
                      PickHit& outHit)
{
    if (!world.IsValid(mesh.Entity) || !options.LayerMask.Intersects(mesh.RenderLayers) ||
        !mesh.Mesh.IsValid() || IsIgnored(mesh.Entity, options))
        return false;
    const auto* worldXf = world.GetComponent<Components::WorldTransform>(mesh.Entity);
    if (!worldXf)
        return false;
    const auto* bounds = world.GetComponent<Components::LocalBounds>(mesh.Entity);
    const auto* sector = world.GetComponent<Components::WorldSectorCoord>(mesh.Entity);

    Components::MeshRenderer synthetic{};
    synthetic.meshGpuHandleId = mesh.Mesh.id;
    synthetic.RenderLayers = mesh.RenderLayers;
    synthetic.modelAssetGuid.Set(mesh.ModelAsset);

    const float32 sectorSize = GetSceneTlas(world).GetSectorSize();
    const Components::WorldTransform effectiveXf =
        Components::ComposeEffectiveWorldTransform(*worldXf, sector, sectorSize);
    return TestEntityHit(ray, mesh.Entity, effectiveXf, synthetic, bounds, options, maxT, outHit);
}

// Restricted-entity variant over the extra meshes: the first entry for the entity that hits.
bool RaycastRestrictedExtraMesh(const Mathematics::Ray3D& ray,
                                ECS::World& world,
                                float32 maxT,
                                const PickOptions& options,
                                const HostRaycastInputs& host,
                                PickHit& outHit)
{
    for (const RaycastMesh& mesh : host.ExtraMeshes)
    {
        if (mesh.Entity == options.RestrictToEntity &&
            RaycastExtraMesh(ray, world, maxT, options, mesh, outHit))
            return true;
    }
    return false;
}

// Lazy: register an AssetEvent callback the first time a ray is cast, so
// hot-reloading or destroying a model invalidates its cached BVH. Lazy
// because the cache is empty until a ray is cast, and EngineCore is
// alive by then.
//
// Uses dispatcher-pointer comparison (not std::call_once) so a recreated
// AssetManager (test harness, project reload) gets the callback re-attached
// on its new dispatcher. The previous registration is implicitly cleaned up
// by the old dispatcher's destruction.
//
// Note on event timing: AssetModified fires on the file-watcher thread BEFORE
// AssetManager::CheckForReloads mutates the asset on the main thread. We
// invalidate eagerly; if a pick races between the invalidate and the actual
// reload, it rebuilds from old vertex data. AssetReloaded then fires after
// the mutation and we invalidate again. Worst case: one wasted build per
// reload. Acceptable.
void EnsureAssetReloadHookRegistered()
{
    static std::atomic<const AssetEventDispatcher*> s_LastDispatcher{nullptr};
    static std::mutex s_RegMutex;

    auto& assetMgr   = EngineCore::GetInstance().GetAssetManager();
    auto& dispatcher = assetMgr.GetEventDispatcher();
    if (s_LastDispatcher.load(std::memory_order_acquire) == &dispatcher)
        return;

    std::lock_guard lock(s_RegMutex);
    if (s_LastDispatcher.load(std::memory_order_relaxed) == &dispatcher)
        return;

    dispatcher.AddCallback([](const AssetEvent& ev)
    {
        if (ev.Type != AssetType::Model || ev.AssetGuid.IsNull())
            return;
        switch (ev.EventType)
        {
            case AssetEventType::AssetModified:
            case AssetEventType::AssetReloaded:
            case AssetEventType::AssetDestroyed:
                // Forward AssetPath so the destroy handler can resolve the
                // disk cache directory after the GUID is erased from m_Assets.
                MeshBvhCache::Instance().InvalidateAsset(ev.AssetGuid, ev.AssetPath);
                break;
            default:
                break;
        }
    });
    s_LastDispatcher.store(&dispatcher, std::memory_order_release);
}

// A terrain surface hit as a scene pick. Terrain carries no triangle identity, so the
// mesh-only fields stay at their "not applicable" defaults.
PickHit MakeTerrainPick(const TerrainPickHit& terrainHit)
{
    PickHit hit{};
    hit.Kind          = PickableKind::Terrain;
    hit.Entity        = terrainHit.Entity;
    hit.SubmeshIndex  = 0u;
    hit.TriangleIndex = ~0u;
    hit.Barycentrics  = Vector3{};
    hit.WorldPosition = terrainHit.WorldPosition;
    hit.WorldNormal   = terrainHit.WorldNormal;
    hit.Distance      = terrainHit.Distance;
    return hit;
}

// The TLAS walk's options for a scene raycast: its reach, layers, disabled rule and ignore list.
TraverseRayOptions MakeTraverseOptions(const PickOptions& options, float32 maxDistance)
{
    TraverseRayOptions traverseOptions;
    traverseOptions.MaxDistance     = maxDistance;
    traverseOptions.LayerMask       = options.LayerMask;
    traverseOptions.IncludeDisabled = options.IncludeDisabled;
    traverseOptions.IgnoreEntities  = options.IgnoreEntities;
    return traverseOptions;
}

// Tests the renderer of one TLAS leaf. False for an entity with no world transform or renderer and
// for one the options skip; else TestEntityHit at the leaf's sector-composed world transform.
bool TestTlasLeaf(const Mathematics::Ray3D& ray,
                  GameEngine::ECS::World& world,
                  const TlasInstance& instance,
                  float32 sectorSize,
                  const PickOptions& options,
                  const HostRaycastInputs& host,
                  float32 maxT,
                  PickHit& outHit)
{
    const auto* worldTransform = world.GetComponent<Components::WorldTransform>(instance.Entity);
    const auto* renderer       = world.GetComponent<Components::MeshRenderer>(instance.Entity);
    if (!worldTransform || !renderer || IsSkipped(world, instance.Entity, host))
        return false;
    const auto* bounds = world.GetComponent<Components::LocalBounds>(instance.Entity);
    const auto* sector = world.GetComponent<Components::WorldSectorCoord>(instance.Entity);
    const Components::WorldTransform effectiveTransform =
        Components::ComposeEffectiveWorldTransform(*worldTransform, sector, sectorSize);
    return TestEntityHit(ray, instance.Entity, effectiveTransform, *renderer, bounds, options, maxT,
                         outHit);
}

// Relative tolerance under which a mesh hit and a terrain hit count as one distance: 1 mm at
// 100 m, a few float32 steps of the distance.
constexpr float32 kSurfaceTieTolerance = 1.0e-5f;

// Whether a terrain hit lies in front of a mesh hit. At the same distance the mesh answers: a mesh
// laid flush on the ground (a road, a floor plate) is what the author put on top of it.
bool TerrainInFront(float32 terrainDistance, float32 meshDistance)
{
    return terrainDistance < meshDistance - kSurfaceTieTolerance * std::max(meshDistance, 1.0f);
}

// The nearest hit so far, and the nearest box entry of a mesh that gave no hit because its BVH
// is pending, which matters only when nothing nearer was hit.
struct NearestHit
{
    float32 BestT = 0.0f;
    float32 NearestPendingT = std::numeric_limits<float32>::max();
    PickResult Result{};

    void Consider(bool hit, const PickHit& candidate)
    {
        if (hit)
        {
            BestT       = candidate.Distance;
            Result.Hit  = true;
            Result.Best = candidate;
        }
        else if (candidate.MeshBvhPending)
            NearestPendingT = std::min(NearestPendingT, candidate.Distance);
    }

    PickResult Finish()
    {
        Result.MeshBvhPending = NearestPendingT < BestT;
        return Result;
    }
};

}

PickResult RaycastSceneForHost(const Mathematics::Ray3D& ray,
                               GameEngine::ECS::World& world,
                               const PickOptions& options,
                               const HostRaycastInputs& host)
{
    GE_CPU_PROFILE_SCOPE("SceneRaycast.RaycastScene");
    EnsureAssetReloadHookRegistered();

    NearestHit nearest;
    nearest.BestT = std::min(options.MaxDistance, std::numeric_limits<float32>::max());

    // Restricted-entity path: single mesh, no terrain, no broad scan.
    if (world.IsValid(options.RestrictToEntity))
    {
        PickHit hit{};
        const bool hitRenderer = RaycastSingleEntity(ray, options.RestrictToEntity, world, nearest.BestT,
                                                     options, host, hit);
        nearest.Consider(hitRenderer, hit);
        if (!hitRenderer)
        {
            PickHit extraHit{};
            nearest.Consider(
                RaycastRestrictedExtraMesh(ray, world, nearest.BestT, options, host, extraHit), extraHit);
        }
        return nearest.Finish();
    }

    // Mesh broad phase: the TLAS walk; TestEntityHit runs once per leaf.
    if (options.IncludeMeshes || options.IncludePrimitives)
    {
        auto& tlas = GetSceneTlas(world);
        if (options.WaitForSceneTlas)
            tlas.WaitCurrent(world); // blocks only when something changed since the last request

        const float32 sectorSize = tlas.GetSectorSize();
        // Leaves come in entry-t order. Once the next leaf's entry t exceeds the best
        // confirmed hit, no further leaf can yield a closer hit.
        auto considerLeaf = [&ray, &world, &options, &host, &nearest, sectorSize](
                                const TlasInstance& instance, float32 tEnter, float32 /*tExit*/) -> bool {
            if (tEnter >= nearest.BestT)
                return false;
            PickHit hit;
            const bool hitLeaf =
                TestTlasLeaf(ray, world, instance, sectorSize, options, host, nearest.BestT, hit);
            nearest.Consider(hitLeaf, hit);
            return true;
        };
        tlas.TraverseRay(ray, MakeTraverseOptions(options, nearest.BestT), considerLeaf);
    }

    if (options.IncludeMeshes)
    {
        for (const RaycastMesh& mesh : host.ExtraMeshes)
        {
            PickHit hit{};
            nearest.Consider(RaycastExtraMesh(ray, world, nearest.BestT, options, mesh, hit), hit);
        }
    }

    if (options.IncludeTerrain)
    {
        TerrainPickHit terrainHit;
        if (RaycastTerrain(ray, world, nearest.BestT, terrainHit) &&
            (nearest.Result.Hit ? TerrainInFront(terrainHit.Distance, nearest.BestT)
                                : terrainHit.Distance < nearest.BestT))
        {
            nearest.BestT = terrainHit.Distance;
            nearest.Result.Hit  = true;
            nearest.Result.Best = MakeTerrainPick(terrainHit);
        }
    }

    return nearest.Finish();
}

bool RaycastSceneAllForHost(const Mathematics::Ray3D& ray,
                            GameEngine::ECS::World& world,
                            std::vector<PickHit>& outHits,
                            const PickOptions& options,
                            const HostRaycastInputs& host)
{
    GE_CPU_PROFILE_SCOPE("SceneRaycast.RaycastSceneAll");
    EnsureAssetReloadHookRegistered();

    outHits.clear();
    bool meshPending = false;

    const float32 maxT = std::min(options.MaxDistance, std::numeric_limits<float32>::max());

    if (options.IncludeMeshes || options.IncludePrimitives)
    {
        auto& tlas = GetSceneTlas(world);
        if (options.WaitForSceneTlas)
            tlas.WaitCurrent(world);

        const float32 sectorSize = tlas.GetSectorSize();
        // Every leaf is visited: the walk collects all hits along the ray.
        auto collectLeaf = [&ray, &world, &options, &host, &outHits, &meshPending, sectorSize, maxT](
                               const TlasInstance& instance, float32 /*tEnter*/, float32 /*tExit*/) -> bool {
            PickHit hit;
            if (TestTlasLeaf(ray, world, instance, sectorSize, options, host, maxT, hit))
                outHits.push_back(hit);
            else
                meshPending |= hit.MeshBvhPending;
            return true;
        };
        tlas.TraverseRay(ray, MakeTraverseOptions(options, maxT), collectLeaf);
    }

    if (options.IncludeMeshes)
    {
        for (const RaycastMesh& mesh : host.ExtraMeshes)
        {
            PickHit hit{};
            if (RaycastExtraMesh(ray, world, maxT, options, mesh, hit))
                outHits.push_back(hit);
            else
                meshPending |= hit.MeshBvhPending;
        }
    }

    std::sort(outHits.begin(), outHits.end(), [](const PickHit& a, const PickHit& b) {
        return a.Distance < b.Distance;
    });

    // Terrain is one surface, so it contributes at most its nearest hit, enough for the
    // click-through stack to step past it onto whatever the ray reaches behind. It goes behind
    // every mesh hit it does not lie in front of: the tie rule of RaycastScene.
    if (options.IncludeTerrain)
    {
        TerrainPickHit terrainHit;
        if (RaycastTerrain(ray, world, maxT, terrainHit))
        {
            const auto firstBehind = std::find_if(outHits.begin(), outHits.end(),
                                                  [&terrainHit](const PickHit& hit) {
                                                      return TerrainInFront(terrainHit.Distance, hit.Distance);
                                                  });
            outHits.insert(firstBehind, MakeTerrainPick(terrainHit));
        }
    }
    return meshPending;
}

PickResult RaycastScene(const Mathematics::Ray3D& ray,
                        GameEngine::ECS::World& world,
                        const PickOptions& options)
{
    return RaycastSceneForHost(ray, world, options, HostRaycastInputs{});
}

bool RaycastSceneAll(const Mathematics::Ray3D& ray,
                     GameEngine::ECS::World& world,
                     std::vector<PickHit>& outHits,
                     const PickOptions& options)
{
    return RaycastSceneAllForHost(ray, world, outHits, options, HostRaycastInputs{});
}

}
