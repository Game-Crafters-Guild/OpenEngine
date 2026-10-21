#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"

#include "NavigationGridBake.h"
#include "NavigationMeshBake.h"
#include "NavigationSourceCollector.h"

#include "PathfindingECS/Components/NavigationObstacle.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"

#include "Assets/AssetManager.h"
#include "Assets/NavGridAsset.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "Logger/Logger.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace GameEngine::PathfindingECS
{

namespace
{

using Components::NavigationBakeFailure;
using Components::NavigationBakePhase;
using Components::NavigationBakeWait;
using Components::NavigationSurface;
using Components::NavigationSurfaceMode;

// ~2 seconds at 60 fps: long enough that a healthy drain never trips it, short enough that a
// wedged or starved gate surfaces while reproducible.
constexpr uint32 kGateStarvationWarnFrames = 120;
// The most cells a grid surface holds (2048 x 2048): about 90 MB of grid data.
constexpr uint64 kMaxGridCells = 2048ull * 2048ull;
// Agents one crowd steers; the crowd is bound to the first baked mesh surface.
constexpr uint32 kMaxCrowdAgents = 128;

// The authored fields a bake depends on, hashed, so a change after the bake shows in the status.
uint64 HashBakeSettings(const NavigationSurface& surface, const Components::WorldTransform* transform)
{
    using Hashing::Fnv1a64Value;
    uint64 hash = Hashing::kFnv1a64OffsetBasis;
    hash = Fnv1a64Value(hash, surface.Mode);
    hash = Fnv1a64Value(hash, surface.GridType);
    hash = Fnv1a64Value(hash, surface.AgentRadius);
    hash = Fnv1a64Value(hash, surface.AgentHeight);
    hash = Fnv1a64Value(hash, surface.MaxSlope);
    hash = Fnv1a64Value(hash, surface.StepHeight);
    hash = Fnv1a64Value(hash, surface.CellSize);
    hash = Fnv1a64Value(hash, surface.FitBounds);
    // Fitted bounds follow the sources; only authored bounds and their placement are settings.
    if (!surface.FitBounds)
    {
        for (const float32 value : {surface.Center.x, surface.Center.y, surface.Center.z, surface.Extent.x,
                                    surface.Extent.y, surface.Extent.z})
            hash = Fnv1a64Value(hash, value);
        if (transform)
            for (size_t i = 12; i < 15; ++i)
                hash = Fnv1a64Value(hash, transform->matrix[i]);
    }
    return Fnv1a64Value(hash, surface.AuthoredCells.ToGuid());
}

// The bake state NavigationSurfaceSystem publishes: everything in the component it writes, nothing
// an author edits.
void CopyBakeStatus(const NavigationSurface& from, NavigationSurface& to)
{
    to.BakePhase = from.BakePhase;
    to.BakeFailure = from.BakeFailure;
    to.BakeWait = from.BakeWait;
    to.BakedMode = from.BakedMode;
    to.BakeDone = from.BakeDone;
    to.BakeTotal = from.BakeTotal;
    to.BakedWalkable = from.BakedWalkable;
    to.BakedBytes = from.BakedBytes;
    to.BakeVersion = from.BakeVersion;
    to.BlockingTileX = from.BlockingTileX;
    to.BlockingTileZ = from.BlockingTileZ;
    to.BlockingModel = from.BlockingModel;
}

Pathfinding::NavMeshSettings MeshSettingsOf(const NavigationSurface& surface)
{
    Pathfinding::NavMeshSettings settings;
    settings.CellSize = surface.CellSize;
    settings.AgentRadius = surface.AgentRadius;
    settings.AgentHeight = surface.AgentHeight;
    settings.MaxSlope = surface.MaxSlope;
    settings.StepHeight = surface.StepHeight;
    return settings;
}

bool SameMesh(const Pathfinding::NavMeshSettings& a, const Pathfinding::NavMeshSettings& b)
{
    return a.CellSize == b.CellSize && a.AgentRadius == b.AgentRadius && a.AgentHeight == b.AgentHeight &&
           a.MaxSlope == b.MaxSlope && a.StepHeight == b.StepHeight;
}

bool SameLayout(const Pathfinding::NavMeshTileLayout& a, const Pathfinding::NavMeshTileLayout& b)
{
    return a.OriginX == b.OriginX && a.OriginY == b.OriginY && a.OriginZ == b.OriginZ && a.TilesX == b.TilesX &&
           a.TilesZ == b.TilesZ;
}

// True when anything the bake reads lies inside the bounds.
bool HasGeometryInside(const NavigationBounds& bounds, const NavigationTerrainSource& terrain,
                       const NavigationStaticGeometry& geometry)
{
    if (bounds.Empty)
        return false;
    if (terrain.OverlapsXZ(bounds.Min[0], bounds.Min[2], bounds.Max[0], bounds.Max[2]))
        return true;
    return std::any_of(geometry.Items.begin(), geometry.Items.end(), [&](const NavigationStaticGeometry::Item& item) {
        return item.Bounds.OverlapsXZ(bounds.Min[0], bounds.Min[2], bounds.Max[0], bounds.Max[2]);
    });
}

uint32 CountWalkableCells(const Pathfinding::GridMap& grid)
{
    const uint8* blocked = grid.GetBlockedData();
    return static_cast<uint32>(std::count(blocked, blocked + grid.GetCellCount(), uint8{0}));
}

// Grid data bytes: cost, height, blocked and dynamic-blocked per cell.
uint64 GridDataBytes(const Pathfinding::GridMap& grid)
{
    return static_cast<uint64>(grid.GetCellCount()) * (sizeof(float32) * 2 + 2);
}

} // namespace

struct NavigationSurfaceSystem::SurfaceRecord
{
    uint64 WorldId = 0;
    uint64 LastUpdate = 0;
    // The map this system last bound the surface to. Undo and redo restore the component's bytes,
    // binding included; a map the restored bytes no longer name was made for this surface alone.
    Pathfinding::NavMapHandle BoundMap;
    // The bake state this system last published, put back over bytes an undo restored.
    bool HasStatus = false;
    NavigationSurface Status;
    // A bake should start as soon as its sources are ready.
    bool Pending = false;
    // SettingsHash holds the settings of the bake in the map (or in progress).
    bool KnowsSettings = false;
    uint64 SettingsHash = 0;
    std::unique_ptr<NavigationMeshBake> MeshBake;
    // The loads the waiting bake requested, by model, until the bake starts.
    struct ModelLoad
    {
        AssetFuture Load;
        bool Reported = false;
    };
    std::unordered_map<GUID, ModelLoad> ModelLoads;
};

NavigationSurfaceSystem::NavigationSurfaceSystem()
    : m_AsyncState(std::make_shared<AsyncState>())
{
}

NavigationSurfaceSystem::~NavigationSurfaceSystem() = default;

void NavigationSurfaceSystem::UpdateAuthoredCellsWatch()
{
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;

    // Cheap per-frame gate. Mounting or rebinding an asset source is the only thing that can move
    // the project root, and both bump this counter, so an unchanged version means the watch is
    // still pointed at the right place.
    const uint64 sourceSetVersion = assetManager->GetSourceSetVersion();
    if (sourceSetVersion == m_WatchedSourceSetVersion)
        return;
    m_WatchedSourceSetVersion = sourceSetVersion;

    const std::filesystem::path assetRoot = assetManager->GetAssetRoot();
    if (assetRoot == m_WatchedAssetRoot)
        return;

    // Release the old root's subscription before taking one on the new root: the editor rebinds
    // the project source in-process when the user opens a different project.
    m_NavGridWatchSub.reset();
    m_WatchedAssetRoot.clear();
    if (assetRoot.empty())
        return;

    // Watching is armed during editor startup, before the first frame ticks. Where it is not armed
    // (player, tests, UI replay) there is nothing to subscribe to; the next mount or rebind retries.
    auto& watchService = FileWatchingService::GetInstance();
    if (!watchService.IsWatching())
        return;

    // The callback touches only the shared state. Destroying a subscription waits for a callback
    // already running, so none runs after the system.
    auto asyncState = m_AsyncState;
    FilePattern gridPattern(assetRoot, ".*", {".navgrid"}, true);
    m_NavGridWatchSub.emplace(watchService.Subscribe(gridPattern, [asyncState](const FileChangeEvent& event) {
        if (event.Type == FileChangeType::Modified)
        {
            std::lock_guard lock(asyncState->Mutex);
            asyncState->AuthoredCellsChanged = true;
        }
    }));
    m_WatchedAssetRoot = assetRoot;
}

void NavigationSurfaceSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;

    // Path solves read maps unlocked (NavigationWorld's thread-safety contract), so every map
    // mutation this system makes — creating, re-baking, applying tiles and dynamic obstacles — must
    // wait until no solve runs. Every trigger (BakeRequested, a pending bake, a mesh bake's
    // finished tiles) persists across frames, so a deferred frame retries on the next one.
    //
    // Solves run only inside NavigationPathfindingSystem's exclusive Update
    // (NavigationWorld::SolveGridPaths), which finishes them before it returns, so
    // HasActivePathJobs() is false whenever this system runs and the gate never fires in the
    // engine's own schedule. It stays the guard for a caller that could overlap a solve.
    if (navWorld->HasActivePathJobs())
    {
        ++m_GateDeferredFrames;
        if (m_GateDeferredFrames == kGateStarvationWarnFrames)
        {
            Logger::Log::Warning("[PathfindingECS] NavigationSurfaceSystem: map updates deferred for {} consecutive "
                                 "frames by running path solves; surfaces are not baking or applying obstacles",
                                 kGateStarvationWarnFrames);
        }
        return;
    }
    m_GateDeferredFrames = 0;
    ++m_UpdateIndex;

    UpdateAuthoredCellsWatch();
    bool authoredCellsChanged = false;
    {
        std::lock_guard lock(m_AsyncState->Mutex);
        std::swap(authoredCellsChanged, m_AsyncState->AuthoredCellsChanged);
    }

    auto& engine = EngineCore::GetInstance();
    JobSystem::WorkStealingThreadPool* pool = engine.IsInitialized() ? &engine.GetJobSystem() : nullptr;

    world.Query<ECS::Write<NavigationSurface>, ECS::Optional<Components::Transform>,
                ECS::Optional<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity, NavigationSurface& surface, const Components::Transform* local,
                  const Components::WorldTransform* transform) {
            if (authoredCellsChanged && surface.Mode == NavigationSurfaceMode::Grid && !surface.AuthoredCells.IsNull())
                surface.BakeRequested = true;
            // A placed surface whose WorldTransform TransformHierarchy has not resolved yet (Version 0)
            // has no position to bake around; one with no Transform at all sits at the origin.
            const bool placed = !local || (transform && transform->Version != 0u);
            UpdateSurface(world, entity, surface, placed ? transform : nullptr, placed, pool);
        });

    // A record not visited this update belongs to a surface that is gone: dropping it abandons its
    // bake's jobs, which own everything they touch.
    std::erase_if(m_Surfaces, [this](const auto& entry) { return entry.second->LastUpdate != m_UpdateIndex; });
    ReleaseUnboundMaps(world);

    // Last step so freshly baked grids also carry this frame's obstacle marks. NavigationPathfindingSystem
    // solves in the next wave, so paths computed this frame see this frame's obstacles.
    ApplyDynamicObstacles(world);
}

void NavigationSurfaceSystem::UpdateSurface(ECS::World& world, ECS::EntityHandle entity, NavigationSurface& surface,
                                            const Components::WorldTransform* transform, bool placed,
                                            JobSystem::WorkStealingThreadPool* pool)
{
    auto& slot = m_Surfaces[entity];
    if (!slot || slot->WorldId != world.GetWorldId())
    {
        slot = std::make_unique<SurfaceRecord>();
        slot->WorldId = world.GetWorldId();
    }
    SurfaceRecord& record = *slot;
    record.LastUpdate = m_UpdateIndex;

    if (record.HasStatus)
        CopyBakeStatus(record.Status, surface);
    const Pathfinding::NavMapHandle bound = ResolveNavigationSurfaceMap(world, surface);
    if (record.BoundMap.IsValid() && record.BoundMap != bound)
    {
        m_Unbound.push_back(record.BoundMap);
        record.BoundMap = {};
    }

    // A surface with no live map bakes as soon as it is seen: a new surface, a loaded scene, a world
    // restored from bytes whose maps were released.
    if (!bound.IsValid())
    {
        ForgetNavigationSurfaceMap(surface);
        record.Pending = true;
    }
    if (surface.BakeRequested)
    {
        surface.BakeRequested = false;
        record.Pending = true;
    }
    // A phase this system is not running came with the component's bytes (an undo restoring a
    // snapshot taken mid-bake, a bake another system instance started): bake again.
    const bool inProgress = surface.BakePhase == NavigationBakePhase::Baking || surface.BakePhase == NavigationBakePhase::Waiting;
    if (inProgress && !record.MeshBake)
        record.Pending = true;
    if (record.Pending && placed)
        StartBake(world, entity, surface, transform, record);
    if (record.MeshBake)
        StepMeshBake(world, entity, surface, pool, record);

    surface.SettingsChanged = record.KnowsSettings && surface.BakePhase == NavigationBakePhase::Baked &&
                              record.SettingsHash != HashBakeSettings(surface, transform);

    record.BoundMap = ResolveNavigationSurfaceMap(world, surface);
    CopyBakeStatus(surface, record.Status);
    record.HasStatus = true;
}

void NavigationSurfaceSystem::ReleaseUnboundMaps(ECS::World& world)
{
    if (m_Unbound.empty())
        return;
    // A map some surface still names stays: an entity copied inside the world binds its
    // original's map until the copy is repaired.
    world.Query<ECS::Read<NavigationSurface>>().Each([&](const NavigationSurface& surface) {
        const auto named = ResolveNavigationSurfaceMap(world, surface);
        std::erase(m_Unbound, named);
    });
    for (const auto handle : m_Unbound)
        ReleaseNavigationMap(handle);
    m_Unbound.clear();
}

void NavigationSurfaceSystem::StartBake(ECS::World& world, ECS::EntityHandle entity, NavigationSurface& surface,
                                        const Components::WorldTransform* transform, SurfaceRecord& record)
{
    const NavigationTerrainSource terrain = ResolveNavigationTerrainSource(world);
    if (terrain.Loading)
    {
        surface.BakePhase = NavigationBakePhase::Waiting;
        surface.BakeWait = NavigationBakeWait::TerrainData;
        return;
    }
    if (const GUID model = FirstLoadingModel(world, entity, record); !model.IsNull())
    {
        surface.BakePhase = NavigationBakePhase::Waiting;
        surface.BakeWait = NavigationBakeWait::Model;
        surface.BlockingModel.Set(model);
        return;
    }
    record.ModelLoads.clear();
    record.Pending = false;
    record.MeshBake.reset();
    record.KnowsSettings = true;
    record.SettingsHash = HashBakeSettings(surface, transform);

    // The map keeps its handle across bakes; a mode change needs the other kind of map.
    const bool rightKind = surface.Mode == NavigationSurfaceMode::Grid ? ResolveNavigationSurfaceGrid(world, surface) != nullptr
                                                                       : ResolveNavigationSurfaceMesh(world, surface) != nullptr;
    if (!rightKind)
    {
        ResetNavigationSurfaceMap(world, surface);
        if (!CreateNavigationSurfaceMap(world, surface).IsValid())
        {
            Logger::Log::Error("[PathfindingECS] Could not create a navigation map for entity {}", entity.id);
            record.Pending = true;
            return;
        }
    }

    auto geometry = std::make_shared<const NavigationStaticGeometry>(GatherNavigationStaticGeometry(world));
    const NavigationBounds bounds = NavigationSurfaceBounds(surface, transform, terrain, *geometry);
    surface.BakeFailure = NavigationBakeFailure::None;
    if (!HasGeometryInside(bounds, terrain, *geometry))
    {
        surface.BakePhase = NavigationBakePhase::Failed;
        surface.BakeFailure = terrain.Spherical ? NavigationBakeFailure::SphericalTerrain : NavigationBakeFailure::NoGeometry;
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                             Navigation::FormatBakeStatus(surface));
        return;
    }

    if (surface.Mode == NavigationSurfaceMode::Grid)
        BakeGrid(world, entity, surface, terrain, *geometry, bounds, record);
    else
        StartMeshBake(world, entity, surface, terrain, std::move(geometry), bounds, record);
}

GUID NavigationSurfaceSystem::FirstLoadingModel(ECS::World& world, ECS::EntityHandle entity, SurfaceRecord& record)
{
    GUID loading;
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return loading;
    for (const GUID& model : FindUnloadedNavigationModels(world))
    {
        auto it = record.ModelLoads.find(model);
        if (it == record.ModelLoads.end())
            it = record.ModelLoads.emplace(model, SurfaceRecord::ModelLoad{assets->LoadAssetAsync(model)}).first;
        SurfaceRecord::ModelLoad& load = it->second;
        if (load.Load.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        {
            if (loading.IsNull())
                loading = model;
            continue;
        }
        // A load that landed after the list was made is read by the gather that follows; only a
        // load that ended without the asset is reported.
        if (!load.Reported && !load.Load.get())
        {
            load.Reported = true;
            Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: model {} could not be loaded; "
                                 "the bake goes ahead without it",
                                 entity.id, model.ToString());
        }
    }
    return loading;
}

void NavigationSurfaceSystem::StartMeshBake(ECS::World& world, ECS::EntityHandle entity, NavigationSurface& surface,
                                            const NavigationTerrainSource& terrain,
                                            std::shared_ptr<const NavigationStaticGeometry> geometry,
                                            const NavigationBounds& bounds, SurfaceRecord& record)
{
    const Pathfinding::NavMeshSettings settings = MeshSettingsOf(surface);
    const NavigationBakeExtent extent = NavigationBakeExtentOf(surface, bounds);
    Pathfinding::NavMeshTileLayout layout;
    layout.OriginX = bounds.Min[0];
    layout.OriginY = bounds.Min[1];
    layout.OriginZ = bounds.Min[2];
    layout.TilesX = extent.X;
    layout.TilesZ = extent.Z;
    const uint64 tiles = extent.Count();
    if (!(settings.CellSize > 0.0f) || tiles > Pathfinding::DetourNavMap::kMaxTiles)
    {
        surface.BakePhase = NavigationBakePhase::Failed;
        surface.BakeFailure = NavigationBakeFailure::TooLarge;
        surface.BakeTotal = static_cast<uint32>(std::min<uint64>(tiles, UINT32_MAX));
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                             Navigation::FormatBakeStatus(surface));
        return;
    }

    auto* mesh = ResolveNavigationSurfaceMesh(world, surface);
    // A bake with the same tiles replaces them one at a time, so the old tiles serve until each new
    // one lands. Different tiles need a fresh mesh, and the crowd steering on the old one stops.
    if (!mesh->IsBuilt() || !SameMesh(mesh->GetSettings(), settings) || !SameLayout(mesh->GetLayout(), layout))
    {
        auto& crowd = NavigationService::Get().GetCrowdManager();
        if (crowd.IsBoundTo(*mesh))
            crowd.Shutdown();
        if (!mesh->Initialize(settings, layout))
        {
            surface.BakePhase = NavigationBakePhase::Failed;
            surface.BakeFailure = NavigationBakeFailure::TooLarge;
            surface.BakeTotal = static_cast<uint32>(tiles);
            return;
        }
    }
    record.MeshBake =
        std::make_unique<NavigationMeshBake>(settings, layout, std::move(geometry), TerrainLatticeStride(terrain, settings.CellSize));
    surface.BakePhase = NavigationBakePhase::Baking;
    surface.BakeDone = 0;
    surface.BakeTotal = static_cast<uint32>(tiles);
}

void NavigationSurfaceSystem::StepMeshBake(ECS::World& world, ECS::EntityHandle entity, NavigationSurface& surface,
                                           JobSystem::WorkStealingThreadPool* pool, SurfaceRecord& record)
{
    NavigationMeshBake& bake = *record.MeshBake;
    auto* mesh = ResolveNavigationSurfaceMesh(world, surface);
    if (!mesh)
    {
        record.MeshBake.reset();
        record.Pending = true;
        return;
    }

    // The terrain's heightfield pointers are main-thread data and can change between frames, so it
    // is resolved again for every update's snapshots.
    const NavigationTerrainSource terrain = ResolveNavigationTerrainSource(world);
    int32 waitX = 0, waitZ = 0;
    const bool submitted = bake.SubmitTiles(terrain, pool, kTileSnapshotsPerUpdate, waitX, waitZ);

    uint32 failedX = 0, failedZ = 0;
    std::string error;
    if (!bake.ApplyFinishedTiles(*mesh, failedX, failedZ, error))
    {
        surface.BakePhase = NavigationBakePhase::Failed;
        surface.BakeFailure = NavigationBakeFailure::TileBuild;
        surface.BlockingTileX = static_cast<int32>(failedX);
        surface.BlockingTileZ = static_cast<int32>(failedZ);
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: tile ({}, {}) did not build: {}",
                             entity.id, failedX, failedZ, error);
        record.MeshBake.reset();
        return;
    }

    surface.BakeDone = bake.GetAppliedTileCount();
    surface.BakeTotal = bake.GetTileCount();
    if (!submitted)
    {
        surface.BakePhase = NavigationBakePhase::Waiting;
        surface.BakeWait = NavigationBakeWait::TerrainTile;
        surface.BlockingTileX = waitX;
        surface.BlockingTileZ = waitZ;
        return;
    }
    surface.BakePhase = NavigationBakePhase::Baking;
    if (!bake.IsComplete())
        return;

    record.MeshBake.reset();
    surface.BakePhase = NavigationBakePhase::Baked;
    surface.BakedMode = NavigationSurfaceMode::Mesh;
    surface.BakedWalkable = mesh->GetPolygonCount();
    surface.BakedBytes = mesh->GetTileDataBytes();
    ++surface.BakeVersion;
    Logger::Log::Info("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                      Navigation::FormatBakeStatus(surface));

    auto& crowd = NavigationService::Get().GetCrowdManager();
    if (!crowd.IsInitialized() && mesh->GetTileCount() > 0)
        crowd.Initialize(*mesh, kMaxCrowdAgents);
}

void NavigationSurfaceSystem::BakeGrid(ECS::World& world, ECS::EntityHandle entity, NavigationSurface& surface,
                                       const NavigationTerrainSource& terrain, const NavigationStaticGeometry& geometry,
                                       const NavigationBounds& bounds, SurfaceRecord& record)
{
    Pathfinding::GridSettings settings;
    settings.Type = surface.GridType;
    settings.CellSize = surface.CellSize;
    settings.OriginX = bounds.Min[0];
    settings.OriginY = bounds.Min[1];
    settings.OriginZ = bounds.Min[2];
    settings.MaxSlope = surface.MaxSlope;
    // A path steps between neighbouring cells, the farthest being diagonal: the step test allows
    // a Max Slope slope's rise over that distance (WalkableClimb).
    settings.MaxStepHeight = Pathfinding::WalkableClimb(surface.StepHeight, surface.MaxSlope,
                                                        std::sqrt(2.0f) * surface.CellSize);
    const bool validCell = surface.CellSize > 0.0f;
    const NavigationBakeExtent extent = NavigationBakeExtentOf(surface, bounds);
    const uint64 cells = extent.Count();
    if (!validCell || cells > kMaxGridCells)
    {
        surface.BakePhase = NavigationBakePhase::Failed;
        surface.BakeFailure = NavigationBakeFailure::TooLarge;
        surface.BakeTotal = static_cast<uint32>(std::min<uint64>(cells, UINT32_MAX));
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                             Navigation::FormatBakeStatus(surface));
        return;
    }
    settings.Width = extent.X;
    settings.Depth = extent.Z;

    int32 waitX = 0, waitZ = 0;
    std::unique_ptr<Pathfinding::GridMap> grid =
        BakeNavigationGrid(settings, terrain, geometry, surface.StepHeight, surface.AgentHeight, waitX, waitZ);
    if (!grid)
    {
        surface.BakePhase = NavigationBakePhase::Waiting;
        surface.BakeWait = NavigationBakeWait::TerrainTile;
        surface.BlockingTileX = waitX;
        surface.BlockingTileZ = waitZ;
        record.Pending = true;
        return;
    }
    ApplyAuthoredCells(surface, entity, *grid);

    const uint32 walkable = CountWalkableCells(*grid);
    const uint64 bytes = GridDataBytes(*grid);
    const uint32 total = grid->GetCellCount();
    NavigationService::Get().ReplaceGridMap(ResolveNavigationSurfaceMap(world, surface), std::move(grid));
    surface.BakePhase = NavigationBakePhase::Baked;
    surface.BakedMode = NavigationSurfaceMode::Grid;
    surface.BakeDone = total;
    surface.BakeTotal = total;
    surface.BakedWalkable = walkable;
    surface.BakedBytes = bytes;
    ++surface.BakeVersion;
    Logger::Log::Info("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                      Navigation::FormatBakeStatus(surface));
}

void NavigationSurfaceSystem::ApplyAuthoredCells(const NavigationSurface& surface, ECS::EntityHandle entity,
                                                 Pathfinding::GridMap& grid)
{
    if (surface.AuthoredCells.IsNull())
        return;
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;
    const GUID guid = surface.AuthoredCells.ToGuid();
    const auto asset = assetManager->GetAsset(guid);
    if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::NavigationGrid)
    {
        // Bake without the painted cells now and again once they have loaded. Reserve the in-flight
        // slot under the lock but call LoadAsset outside it: LoadAsset may run the callback inline,
        // and the callback takes the same (non-recursive) lock.
        bool startLoad = false;
        {
            std::lock_guard lock(m_AsyncState->Mutex);
            startLoad = m_AsyncState->Loading.insert(guid).second;
        }
        if (startLoad)
        {
            assetManager->LoadAsset(guid, [asyncState = m_AsyncState, guid](Result<SharedPtr<Asset>, AssetError> result) {
                std::lock_guard lock(asyncState->Mutex);
                asyncState->Loading.erase(guid);
                if (result.IsOk() && result.Value() && result.Value()->IsLoaded())
                    asyncState->AuthoredCellsChanged = true;
                else
                    Logger::Log::Error("[PathfindingECS] Could not load the authored cells (.navgrid) of a navigation "
                                       "surface: {}",
                                       result.IsOk() ? "asset not loaded" : ToString(result.Error()));
            });
        }
        return;
    }

    const auto& authored = static_cast<const NavGridAsset&>(*asset);
    const auto& gridSettings = grid.GetSettings();
    const auto& authoredSettings = authored.GetGridSettings();
    if (authoredSettings.Width != gridSettings.Width || authoredSettings.Depth != gridSettings.Depth)
    {
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: its authored cells are {} x {} and the "
                             "grid is {} x {}, so they are not applied; paint them again for the current bounds",
                             entity.id, authoredSettings.Width, authoredSettings.Depth, gridSettings.Width,
                             gridSettings.Depth);
        return;
    }
    const auto& costs = authored.GetCosts();
    const auto& blocked = authored.GetBlocked();
    for (uint32 i = 0; i < grid.GetCellCount(); ++i)
    {
        uint32 cx = 0, cz = 0;
        grid.CellFromIndex(i, cx, cz);
        if (i < costs.size())
            grid.SetCellCost(cx, cz, costs[i]);
        if (i < blocked.size() && blocked[i])
            grid.SetCellBlocked(cx, cz, true);
    }
}

void NavigationSurfaceSystem::ApplyDynamicObstacles(ECS::World& world)
{
    world.Query<ECS::Read<NavigationSurface>>().Each([&](ECS::EntityHandle /*e*/, const NavigationSurface& surface) {
        if (auto* grid = ResolveNavigationSurfaceGrid(world, surface))
            grid->ClearAllDynamicBlocked();
    });

    // Rasterize each obstacle's footprint into every grid it overlaps.
    world.Query<ECS::Read<Components::NavigationObstacle>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle /*e*/, const Components::NavigationObstacle& obstacle, const Components::WorldTransform& wt) {
            const float32 posX = wt.matrix[12];
            const float32 posZ = wt.matrix[14];
            const float32 radius = obstacle.Radius;

            world.Query<ECS::Read<NavigationSurface>>().Each([&](ECS::EntityHandle /*e*/, const NavigationSurface& surface) {
                auto* gridMap = ResolveNavigationSurfaceGrid(world, surface);
                if (!gridMap)
                    return;

                uint32 centerX = 0, centerZ = 0;
                if (!gridMap->WorldToCell(posX, posZ, centerX, centerZ))
                    return;

                const auto& gridSettings = gridMap->GetSettings();
                const float32 cellSize = gridSettings.CellSize;

                // Radius is authored and unvalidated, and this stamp is O(cellRadius^2) per obstacle per
                // grid, every frame: Radius=1000 at CellSize=0.1 is 4e8 iterations. Clamping the cell
                // radius bounds that loop.
                //
                // Clamp to Width + Depth, not to max(Width, Depth). The farthest in-grid cell from an
                // in-grid centre is up to (Width-1)^2 + (Depth-1)^2 away, which exceeds max(Width, Depth)^2
                // for any square grid of 4 cells or more — clamping to the longer side would drop the far
                // corners of an obstacle that really does span the grid. (Width + Depth)^2 exceeds that
                // distance for every grid, so the loop is bounded and the footprint is unchanged.
                //
                // A non-positive radius blocks nothing, including the obstacle's own cell. This also
                // covers NaN, whose only other route is static_cast<uint32>(NaN) — undefined.
                if (!(radius > 0.0f))
                    return;
                // Clamped before the cast: radius/cellSize is inf for CellSize=0, and casting that to an
                // integer is undefined.
                const float32 maxCellRadius =
                    static_cast<float32>(gridSettings.Width) + static_cast<float32>(gridSettings.Depth);
                const int32 r = static_cast<int32>(std::min(std::ceil(radius / cellSize), maxCellRadius));
                // 64-bit: dx^2 + dz^2 reaches 2r^2, which leaves int32 range for a grid past ~32k cells on
                // a side.
                const int64 rSq = static_cast<int64>(r) * r;

                for (int32 dz = -r; dz <= r; ++dz)
                {
                    for (int32 dx = -r; dx <= r; ++dx)
                    {
                        if (static_cast<int64>(dx) * dx + static_cast<int64>(dz) * dz > rSq)
                            continue;

                        const int32 cx = static_cast<int32>(centerX) + dx;
                        const int32 cz = static_cast<int32>(centerZ) + dz;
                        if (cx < 0 || cz < 0)
                            continue;

                        gridMap->SetCellDynamicBlocked(static_cast<uint32>(cx), static_cast<uint32>(cz), true);
                    }
                }
            });
        });
}

} // namespace GameEngine::PathfindingECS
