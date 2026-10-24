#include "PathfindingECS/Systems/NavigationSurfaceSystem.h"

#include "NavigationBakedData.h"
#include "NavigationGridBake.h"
#include "NavigationMeshBake.h"
#include "NavigationSourceCollector.h"

#include "Components/Navigation/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "Components/Navigation/NavigationSurface.h"
#include "PathfindingECS/Navigation.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"

#include "Assets/AssetManager.h"
#include "Assets/NavGridAsset.h"
#include "Assets/NavSurfaceAsset.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "Logger/Logger.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavSurfaceFile.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/PathBuffer.h"
#include "ECS/ChangeFilter.h"
#include "TerrainECS/PlanarHeightfieldEditCursor.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

namespace GameEngine::PathfindingECS
{

namespace
{

using Components::NavigationBakeFailure;
using Components::NavigationBakePhase;
using Components::NavigationBakeWait;
using Components::NavigationSurface;
using Components::NavigationSurfaceMode;

static_assert(Pathfinding::kNavChartSectorSize == Components::kWorldSectorSize,
              "a chart origin's sector is a world sector");

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
    to.MapHoldsBake = from.MapHoldsBake;
    to.BakedMode = from.BakedMode;
    to.BakeDone = from.BakeDone;
    to.BakeTotal = from.BakeTotal;
    to.BakedWalkable = from.BakedWalkable;
    to.BakedBytes = from.BakedBytes;
    to.BakeVersion = from.BakeVersion;
    to.StaleTiles = from.StaleTiles;
    to.TilesBuilt = from.TilesBuilt;
    to.TilesDirty = from.TilesDirty;
    to.TilesBaking = from.TilesBaking;
    to.BlockingTileX = from.BlockingTileX;
    to.BlockingTileZ = from.BlockingTileZ;
    to.BlockingPoint = from.BlockingPoint;
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

// The chart a surface bakes into: one per surface, its origin at the bounds' minimum corner, so
// the surface's tiles run from (0, 0) and a wider surface than one chart's span is refused.
Pathfinding::NavMeshChart ChartOf(const NavigationBounds& bounds, const Pathfinding::NavMeshSettings& settings)
{
    Pathfinding::NavMeshChart chart;
    chart.Origin = Pathfinding::ChartOrigin::AtWorld(bounds.Min[0], bounds.Min[1], bounds.Min[2]);
    chart.Span = static_cast<float32>(Pathfinding::kNavChartTilesPerSide) *
                 Pathfinding::DetourNavMap::GetTileSize(settings);
    return chart;
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

// A rectangle in world metres: X along world X, Y along world Z.
using WorldRect = Mathematics::Rect;

// Grid data bytes: cost, height, blocked and dynamic-blocked per cell.
uint64 GridDataBytes(const Pathfinding::GridMap& grid)
{
    return static_cast<uint64>(grid.GetCellCount()) * (sizeof(float32) * 2 + 2);
}

using Carves = std::vector<std::pair<ECS::EntityHandle, NavigationCarve>>;

// The carving obstacles in `world`, by entity: the cylinder each cuts.
void GatherCarves(ECS::World& world, Carves& carves)
{
    carves.clear();
    world.Query<ECS::Read<Components::NavigationObstacle>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::NavigationObstacle& obstacle,
            const Components::WorldTransform& transform) {
            if (!obstacle.Carve || !(obstacle.Radius > 0.0f))
                return;
            const float32 y = transform.matrix[13];
            carves.push_back({entity, {transform.matrix[12], transform.matrix[14], obstacle.Radius,
                                       y - obstacle.Height, y + obstacle.Height}});
        });
    std::sort(carves.begin(), carves.end(), [](const auto& a, const auto& b) { return a.first.id < b.first.id; });
}

std::shared_ptr<const std::vector<NavigationCarve>> CarveCylinders(const Carves& carves)
{
    auto cylinders = std::make_shared<std::vector<NavigationCarve>>();
    for (const auto& [entity, carve] : carves)
        cylinders->push_back(carve);
    return cylinders;
}

WorldRect RectOf(const NavigationCarve& carve)
{
    return WorldRect::FromEdges(carve.X - carve.Radius, carve.Z - carve.Radius, carve.X + carve.Radius,
                                carve.Z + carve.Radius);
}

// The rectangles of the carves added, removed or changed between `before` and `after`: a changed
// carve dirties where it was and where it is.
void AppendChangedCarves(const Carves& before, const Carves& after, std::vector<WorldRect>& out)
{
    size_t i = 0, j = 0;
    while (i < before.size() || j < after.size())
    {
        if (j == after.size() || (i < before.size() && before[i].first.id < after[j].first.id))
            out.push_back(RectOf(before[i++].second));
        else if (i == before.size() || after[j].first.id < before[i].first.id)
            out.push_back(RectOf(after[j++].second));
        else
        {
            if (!(before[i].second == after[j].second))
            {
                out.push_back(RectOf(before[i].second));
                out.push_back(RectOf(after[j].second));
            }
            ++i;
            ++j;
        }
    }
}

// True when a segment's XZ box touches the rectangle.
bool SegmentTouches(const Pathfinding::PathPoint& a, const Pathfinding::PathPoint& b, const WorldRect& rect)
{
    return std::max(a.X, b.X) >= rect.X && std::min(a.X, b.X) <= rect.Right() && std::max(a.Z, b.Z) >= rect.Y &&
           std::min(a.Z, b.Z) <= rect.Bottom();
}

// Asks every agent on `map` whose path crosses one of the `swapped` tiles to solve its path again;
// it keeps walking the old one until the new one lands (ReplanRequested). Crowd agents check their
// own corridor every step.
void ReplanAgentsCrossing(ECS::World& world, Pathfinding::NavMapHandle map, const Pathfinding::NavMeshSettings& settings,
                          const Pathfinding::NavMeshChart& chart, const std::vector<Pathfinding::NavTileKey>& swapped)
{
    std::vector<WorldRect> rects;
    for (const Pathfinding::NavTileKey& key : swapped)
    {
        float32 local[2][3] = {};
        Pathfinding::DetourNavMap::GetTileBounds(settings, key.X, key.Z, local[0][0], local[0][2], local[1][0],
                                                 local[1][2]);
        float64 low[3];
        float64 high[3];
        chart.ToWorld(local[0], low);
        chart.ToWorld(local[1], high);
        rects.push_back(WorldRect::FromEdges(static_cast<float32>(low[0]), static_cast<float32>(low[2]),
                                             static_cast<float32>(high[0]), static_cast<float32>(high[2])));
    }
    const Pathfinding::PathBuffer& paths = NavigationService::Get().GetPathBuffer();
    world.Query<ECS::Write<Components::NavigationAgentState>>().Each(
        [&](Components::NavigationAgentState& state) {
            if (!state.HasDestination || state.ReplanRequested ||
                Pathfinding::NavMapHandle{state.NavMapIndex, state.NavMapGeneration} != map)
                return;
            const Pathfinding::PathHandle path{state.PathIndex, state.PathGeneration};
            const uint32 count = path.IsValid() ? paths.GetPointCount(path) : 0u;
            for (uint32 i = 1; i < count && !state.ReplanRequested; ++i)
            {
                const Pathfinding::PathPoint a = paths.GetPoint(path, i - 1);
                const Pathfinding::PathPoint b = paths.GetPoint(path, i);
                state.ReplanRequested = std::any_of(rects.begin(), rects.end(),
                                                    [&](const WorldRect& rect) { return SegmentTouches(a, b, rect); });
            }
        });
}

// Asks every agent on `map` that walks a path to solve it again, after a bake replaced the map's
// content; it keeps walking the old path until the new one lands (ReplanRequested).
void ReplanAgentsOn(ECS::World& world, Pathfinding::NavMapHandle map)
{
    world.Query<ECS::Write<Components::NavigationAgentState>>().Each(
        [&](Components::NavigationAgentState& state) {
            if (state.HasDestination && !state.PathDirty &&
                Pathfinding::NavMapHandle{state.NavMapIndex, state.NavMapGeneration} == map &&
                (state.Status == Pathfinding::PathStatus::Complete || state.Status == Pathfinding::PathStatus::Partial))
                state.ReplanRequested = true;
        });
}

// Keeps a CPU window of finest pages at `point` for the next update when the terrain pages its
// height (RequestSurfaceHeightWindow): a bake waiting there asks on every update it waits. False,
// asking nothing, when the terrain's pages have no source yet (a height store still cooking).
bool KeepTerrainHeightWindow(const NavigationTerrainSource& terrain, const Mathematics::Vector3& point)
{
    uint32 pageX = 0;
    uint32 pageZ = 0;
    if (!terrain.Query.FinestPageAt(point.x, point.z, pageX, pageZ))
        return false;
    TerrainECS::RequestSurfaceHeightWindow(point);
    return true;
}

// Holds `surface`'s bake on the terrain height at `point`, which is not on the CPU yet: on the
// level-0 page there, or on the terrain's height data while its pages have no source.
void WaitForTerrainHeight(NavigationSurface& surface, const NavigationTerrainSource& terrain,
                          const Mathematics::Vector3& point)
{
    surface.BakePhase = NavigationBakePhase::Waiting;
    if (!KeepTerrainHeightWindow(terrain, point))
    {
        surface.BakeWait = NavigationBakeWait::TerrainData;
        return;
    }
    surface.BakeWait = NavigationBakeWait::TerrainPage;
    surface.BlockingPoint = point;
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
    // The tiles the mesh's last bake laid out from (0, 0): a bake over the same chart and extent
    // replaces them in place.
    uint32 MeshTilesX = 0;
    uint32 MeshTilesZ = 0;
    // An asset load the waiting bake requested.
    struct AssetLoad
    {
        AssetFuture Load;
        bool Reported = false;
    };
    // The models the waiting bake reads that it requested, until the bake starts.
    std::unordered_map<GUID, AssetLoad> ModelLoads;
    // The pending bake was asked for (RequestBake): it builds every tile and loads none from
    // BakedData.
    bool ExplicitBake = false;
    // The load of BakedData the waiting bake requested, and the asset it is for.
    GUID BakedDataGuid;
    std::optional<AssetLoad> BakedDataLoad;

    // What the mesh holds, for turning a world rectangle into its tiles: set by a bake, or read
    // back from a mesh this record did not bake.
    bool MeshKnown = false;
    Pathfinding::NavMeshSettings MeshSettings;
    Pathfinding::NavMeshChart MeshChart;
    uint32 TerrainStride = 1;
    // The dirty queue: tiles to rebake, oldest first, each once. A tile marked again while its
    // rebake runs is queued again: its snapshot is older than the change.
    std::deque<Pathfinding::NavTileKey> DirtyQueue;
    std::unordered_set<Pathfinding::NavTileKey, Pathfinding::NavTileKeyHash> Dirty;
    // Tiles loaded from BakedData whose content changed since; each is in the queue too.
    std::unordered_set<Pathfinding::NavTileKey, Pathfinding::NavTileKeyHash> Stale;
    // The rebake in flight: at most kTileRebakesPerUpdate tiles.
    std::unique_ptr<NavigationMeshBake> Rebake;
    // The terrain edits seen so far.
    TerrainECS::PlanarHeightfieldEditCursor TerrainEdits;
    // The carving obstacles the mesh was last built with, as of the CarveWatch version.
    Carves BuiltCarves;
    uint64 CarvesVersion = 0;
    // The static geometry the dirty tiles rebake with: the bake's, gathered again when a MarkDirty
    // box arrives, the one signal that geometry the surface does not watch changed.
    std::shared_ptr<const NavigationStaticGeometry> Geometry;
    // MarkDirty boxes kept until the surface is baked and takes them.
    std::vector<WorldRect> PendingBoxes;

    // Queues every tile of the mesh whose rectangle, widened by the border its build reads, touches
    // `rect`.
    void MarkDirty(const WorldRect& rect)
    {
        const float32 tileSize = Pathfinding::DetourNavMap::GetTileSize(MeshSettings);
        const float32 border = Pathfinding::DetourNavMap::GetTileBorder(MeshSettings);
        float32 low[3];
        float32 high[3];
        MeshChart.ToChart(rect.X, 0.0, rect.Y, low);
        MeshChart.ToChart(rect.Right(), 0.0, rect.Bottom(), high);
        const auto tileAt = [tileSize](float32 local) { return static_cast<int32>(std::floor(local / tileSize)); };
        const int32 x0 = std::max(0, tileAt(low[0] - border));
        const int32 z0 = std::max(0, tileAt(low[2] - border));
        const int32 x1 = std::min(static_cast<int32>(MeshTilesX) - 1, tileAt(high[0] + border));
        const int32 z1 = std::min(static_cast<int32>(MeshTilesZ) - 1, tileAt(high[2] + border));
        for (int32 z = z0; z <= z1; ++z)
            for (int32 x = x0; x <= x1; ++x)
                Queue({MeshChart.Id, x, z});
    }

    void Queue(const Pathfinding::NavTileKey& key)
    {
        if (Dirty.insert(key).second)
            DirtyQueue.push_back(key);
    }
};

struct NavigationSurfaceSystem::CarveWatch
{
    uint64 WorldId = 0;
    bool Primed = false;
    std::size_t Structure = 0;
    ECS::ChangeGate ObstacleGate;
    ECS::ChangeGate TransformGate;
    // Counts the changes of Carves.
    uint64 Version = 0;
    Carves Current;
    Carves Scratch;

    // Gathers the carves again when an obstacle was added, removed, changed or moved since the last
    // update, and counts a change when they differ.
    void Update(ECS::World& world)
    {
        // Entry-sampled before the queries, as ECS/ChangeFilter.h requires.
        const uint64 entryVersion = world.GetGlobalSystemVersion();
        const std::size_t structure = world.GetStructuralChangeVersion();
        if (WorldId != world.GetWorldId())
        {
            // Version keeps counting across worlds, so no surface's count from another world matches.
            const uint64 version = Version + 1;
            *this = {};
            Version = version;
            WorldId = world.GetWorldId();
        }
        bool changed = !Primed || !ECS::ChangeFilter::Enabled() || structure != Structure;
        if (!changed)
        {
            auto obstacles = world.Query<ECS::Read<Components::NavigationObstacle>>();
            obstacles.Changed<Components::NavigationObstacle>(ObstacleGate);
            obstacles.Each([&](const Components::NavigationObstacle&) { changed = true; });
            auto moved = world.Query<ECS::Read<Components::WorldTransform>>();
            moved.With<Components::NavigationObstacle>();
            moved.Changed<Components::WorldTransform>(TransformGate);
            moved.Each([&](const Components::WorldTransform&) { changed = true; });
        }
        ObstacleGate.LastRunVersion = entryVersion;
        TransformGate.LastRunVersion = entryVersion;
        Structure = structure;
        Primed = true;
        if (!changed)
            return;
        GatherCarves(world, Scratch);
        if (Scratch != Current)
        {
            std::swap(Scratch, Current);
            ++Version;
        }
    }
};

NavigationSurfaceSystem::NavigationSurfaceSystem()
    : m_CarveWatch(std::make_unique<CarveWatch>()), m_AsyncState(std::make_shared<AsyncState>())
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
    m_CarveWatch->Update(world);
    m_DirtyBoxes.clear();
    NavigationService::TakeDirtyBoxes(world.GetWorldId(), m_DirtyBoxes);
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
    for (const auto& [boxSurface, box] : m_DirtyBoxes)
        if (boxSurface == entity)
            record.PendingBoxes.push_back(box);
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
        record.ExplicitBake = true;
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
    if (surface.Mode == NavigationSurfaceMode::Mesh && surface.BakedMode == NavigationSurfaceMode::Mesh &&
        surface.BakePhase == NavigationBakePhase::Baked && !record.MeshBake && !record.Pending)
        UpdateDirtyTiles(world, entity, surface, transform, pool, record);

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
    std::shared_ptr<const Pathfinding::NavSurfaceData> bakedData;
    if (surface.Mode == NavigationSurfaceMode::Mesh && !record.ExplicitBake && !surface.BakedData.IsNull())
    {
        bool loading = false;
        bakedData = ResolveBakedData(entity, surface, record, loading);
        if (loading)
        {
            surface.BakePhase = NavigationBakePhase::Waiting;
            surface.BakeWait = NavigationBakeWait::BakedData;
            return;
        }
    }
    record.ModelLoads.clear();
    record.BakedDataLoad.reset();
    record.ExplicitBake = false;
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
        StartMeshBake(world, entity, surface, terrain, std::move(geometry), bounds, std::move(bakedData), record);
}

std::shared_ptr<const Pathfinding::NavSurfaceData> NavigationSurfaceSystem::ResolveBakedData(
    ECS::EntityHandle entity, const NavigationSurface& surface, SurfaceRecord& record, bool& outLoading)
{
    outLoading = false;
    AssetManager* assets = EngineCore::GetInstance().TryGetAssetManager();
    if (!assets)
        return nullptr;
    const GUID guid = surface.BakedData.ToGuid();
    if (record.BakedDataGuid != guid)
    {
        record.BakedDataGuid = guid;
        record.BakedDataLoad.reset();
    }
    if (const auto asset = assets->GetAsset(guid); asset && asset->IsLoaded() &&
                                                    asset->GetType() == AssetType::NavigationSurfaceData)
        return static_cast<const NavSurfaceAsset&>(*asset).GetData();
    if (!record.BakedDataLoad)
        record.BakedDataLoad = SurfaceRecord::AssetLoad{assets->LoadAssetAsync(guid)};
    SurfaceRecord::AssetLoad& load = *record.BakedDataLoad;
    if (load.Load.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        outLoading = true;
        return nullptr;
    }
    if (const auto asset = assets->GetAsset(guid); load.Load.get() && asset && asset->IsLoaded() &&
                                                    asset->GetType() == AssetType::NavigationSurfaceData)
        return static_cast<const NavSurfaceAsset&>(*asset).GetData();
    if (!load.Reported)
    {
        load.Reported = true;
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: its baked data {} could not be "
                             "loaded; the surface bakes instead",
                             entity.id, guid.ToString());
    }
    return nullptr;
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
            it = record.ModelLoads.emplace(model, SurfaceRecord::AssetLoad{assets->LoadAssetAsync(model)}).first;
        SurfaceRecord::AssetLoad& load = it->second;
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
                                            const NavigationBounds& bounds,
                                            std::shared_ptr<const Pathfinding::NavSurfaceData> bakedData,
                                            SurfaceRecord& record)
{
    const Pathfinding::NavMeshSettings settings = MeshSettingsOf(surface);
    const NavigationBakeExtent extent = NavigationBakeExtentOf(surface, bounds);
    const uint64 tiles = extent.Count();
    if (!(settings.CellSize > 0.0f) || extent.X > Pathfinding::kNavChartTilesPerSide ||
        extent.Z > Pathfinding::kNavChartTilesPerSide)
    {
        surface.BakePhase = NavigationBakePhase::Failed;
        surface.BakeFailure = NavigationBakeFailure::TooLarge;
        surface.BakeTotal = static_cast<uint32>(std::min<uint64>(tiles, UINT32_MAX));
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                             Navigation::FormatBakeStatus(surface));
        return;
    }

    auto* mesh = ResolveNavigationSurfaceMesh(world, surface);
    const Pathfinding::NavMeshChart chart = ChartOf(bounds, settings);
    // A bake builds every tile from the current state: it supersedes the dirty queue, and the
    // terrain edits and the carves before it are in it.
    record.DirtyQueue.clear();
    record.Dirty.clear();
    record.Stale.clear();
    record.Rebake.reset();
    record.TerrainEdits.Reset(terrain.Query);
    record.BuiltCarves = m_CarveWatch->Current;
    record.CarvesVersion = m_CarveWatch->Version;
    record.Geometry = geometry;
    surface.TilesDirty = 0;
    surface.TilesBaking = 0;
    // A bake with the same tiles replaces them one at a time, so the old tiles serve until each new
    // one lands. Different tiles need a fresh mesh, and the crowd steering on the old one stops.
    if (!mesh->IsBuilt() || !SameMesh(mesh->GetSettings(), settings) || !(mesh->GetChart() == chart) ||
        record.MeshTilesX != extent.X || record.MeshTilesZ != extent.Z)
    {
        auto& crowd = NavigationService::Get().GetCrowdManager();
        if (crowd.IsBoundTo(*mesh))
            crowd.Shutdown();
        record.MeshTilesX = extent.X;
        record.MeshTilesZ = extent.Z;
        surface.MapHoldsBake = false;
        if (!mesh->Initialize(settings, chart))
        {
            surface.BakePhase = NavigationBakePhase::Failed;
            surface.BakeFailure = NavigationBakeFailure::TooLarge;
            surface.BakeTotal = static_cast<uint32>(tiles);
            return;
        }
    }
    record.MeshKnown = true;
    record.MeshSettings = settings;
    record.MeshChart = chart;
    record.TerrainStride = TerrainLatticeStride(terrain, settings.CellSize);
    std::vector<Pathfinding::NavTileKey> keys;
    keys.reserve(tiles);
    for (uint32 z = 0; z < extent.Z; ++z)
        for (uint32 x = 0; x < extent.X; ++x)
            keys.push_back({chart.Id, static_cast<int32>(x), static_cast<int32>(z)});
    record.MeshBake = std::make_unique<NavigationMeshBake>(settings, chart, std::move(keys), std::move(geometry),
                                                           CarveCylinders(record.BuiltCarves), record.TerrainStride);
    if (bakedData && NavigationBakedDataMatches(*bakedData, surface, chart))
        record.MeshBake->UseBakedTiles(std::move(bakedData));
    else if (bakedData)
        Logger::Log::Info("[PathfindingECS] Navigation surface on entity {}: its baked data was made for other "
                          "settings or bounds; the surface bakes",
                          entity.id);
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
    Mathematics::Vector3 waitPoint;
    const bool submitted = bake.SubmitTiles(terrain, pool, kTileSnapshotsPerUpdate, waitPoint);

    std::vector<Pathfinding::NavTileKey> applied;
    Pathfinding::NavTileKey failed;
    std::string error;
    const bool built = bake.ApplyFinishedTiles(*mesh, applied, failed, error);
    surface.BakeVersion += applied.size();
    if (!built)
    {
        surface.BakePhase = NavigationBakePhase::Failed;
        surface.BakeFailure = NavigationBakeFailure::TileBuild;
        surface.BlockingTileX = failed.X;
        surface.BlockingTileZ = failed.Z;
        Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: tile ({}, {}) did not build: {}",
                             entity.id, failed.X, failed.Z, error);
        record.MeshBake.reset();
        return;
    }

    surface.BakeDone = bake.GetFinishedTileCount();
    surface.BakeTotal = bake.GetTileCount();
    if (!submitted)
    {
        WaitForTerrainHeight(surface, terrain, waitPoint);
        return;
    }
    surface.BakePhase = NavigationBakePhase::Baking;
    if (!bake.IsComplete())
        return;

    surface.TilesBuilt += bake.GetBuiltTileCount();
    // A stale tile serves as it is until its rebake, which the dirty queue runs.
    for (const Pathfinding::NavTileKey& key : bake.GetStaleTiles())
    {
        record.Stale.insert(key);
        record.Queue(key);
    }
    surface.StaleTiles = static_cast<uint32>(record.Stale.size());
    surface.TilesDirty = static_cast<uint32>(record.DirtyQueue.size());
    if (!bake.GetStaleTiles().empty())
    {
        surface.BlockingTileX = bake.GetStaleTiles().front().X;
        surface.BlockingTileZ = bake.GetStaleTiles().front().Z;
    }
    record.MeshBake.reset();
    surface.BakePhase = NavigationBakePhase::Baked;
    surface.MapHoldsBake = true;
    surface.BakedMode = NavigationSurfaceMode::Mesh;
    surface.BakedWalkable = mesh->GetPolygonCount();
    surface.BakedBytes = mesh->GetTileDataBytes();
    ReplanAgentsOn(world, ResolveNavigationSurfaceMap(world, surface));
    Logger::Log::Info("[PathfindingECS] Navigation surface on entity {}: {}", entity.id,
                      Navigation::FormatBakeStatus(surface));

    auto& crowd = NavigationService::Get().GetCrowdManager();
    if (!crowd.IsInitialized() && mesh->GetTileCount() > 0)
        crowd.Initialize(*mesh, kMaxCrowdAgents);
}

void NavigationSurfaceSystem::UpdateDirtyTiles(ECS::World& world, ECS::EntityHandle entity, NavigationSurface& surface,
                                               const Components::WorldTransform* transform,
                                               JobSystem::WorkStealingThreadPool* pool, SurfaceRecord& record)
{
    auto* mesh = ResolveNavigationSurfaceMesh(world, surface);
    if (!mesh || !mesh->IsBuilt())
        return;
    const NavigationTerrainSource terrain = ResolveNavigationTerrainSource(world);
    if (!record.MeshKnown)
    {
        // A mesh another system instance baked: its chart and agent are the mesh's own, its extent
        // the surface's bounds now, and what it was built from is taken as the current state.
        record.Geometry = std::make_shared<const NavigationStaticGeometry>(GatherNavigationStaticGeometry(world));
        const NavigationBakeExtent extent =
            NavigationBakeExtentOf(surface, NavigationSurfaceBounds(surface, transform, terrain, *record.Geometry));
        record.MeshKnown = true;
        record.MeshSettings = mesh->GetSettings();
        record.MeshChart = mesh->GetChart();
        record.MeshTilesX = extent.X;
        record.MeshTilesZ = extent.Z;
        record.TerrainStride = TerrainLatticeStride(terrain, record.MeshSettings.CellSize);
        record.TerrainEdits.Reset(terrain.Query);
        record.BuiltCarves = m_CarveWatch->Current;
        record.CarvesVersion = m_CarveWatch->Version;
    }

    std::vector<WorldRect> touched;
    record.TerrainEdits.CollectEdits(terrain.Query, touched);
    if (record.CarvesVersion != m_CarveWatch->Version)
    {
        AppendChangedCarves(record.BuiltCarves, m_CarveWatch->Current, touched);
        record.BuiltCarves = m_CarveWatch->Current;
        record.CarvesVersion = m_CarveWatch->Version;
    }
    if (!record.PendingBoxes.empty())
    {
        record.Geometry = std::make_shared<const NavigationStaticGeometry>(GatherNavigationStaticGeometry(world));
        touched.insert(touched.end(), record.PendingBoxes.begin(), record.PendingBoxes.end());
        record.PendingBoxes.clear();
    }
    for (const WorldRect& rect : touched)
        record.MarkDirty(rect);

    if (!record.Rebake && !record.DirtyQueue.empty())
    {
        std::vector<Pathfinding::NavTileKey> keys;
        while (!record.DirtyQueue.empty() && keys.size() < kTileRebakesPerUpdate)
        {
            keys.push_back(record.DirtyQueue.front());
            record.Dirty.erase(record.DirtyQueue.front());
            record.DirtyQueue.pop_front();
        }
        record.Rebake = std::make_unique<NavigationMeshBake>(record.MeshSettings, record.MeshChart, std::move(keys),
                                                             record.Geometry, CarveCylinders(record.BuiltCarves),
                                                             record.TerrainStride);
        // A tile is dirty when something under it may have changed; its hash says whether it did.
        record.Rebake->SkipUnchangedTiles(*mesh);
    }

    std::vector<Pathfinding::NavTileKey> swapped;
    if (record.Rebake)
    {
        // A tile on terrain height not on the CPU yet waits in the rebake and is submitted again next
        // update, with a CPU window kept there.
        Mathematics::Vector3 waitPoint;
        if (!record.Rebake->SubmitTiles(terrain, pool, kTileRebakesPerUpdate, waitPoint))
            KeepTerrainHeightWindow(terrain, waitPoint);
        Pathfinding::NavTileKey failed;
        std::string error;
        const uint32 builtBefore = record.Rebake->GetBuiltTileCount();
        // A tile that does not rebuild keeps its old tile serving; the rest of its batch goes in.
        if (!record.Rebake->ApplyFinishedTiles(*mesh, swapped, failed, error))
            Logger::Log::Warning("[PathfindingECS] Navigation surface on entity {}: tile ({}, {}) did not rebake: {}",
                                 entity.id, failed.X, failed.Z, error);
        surface.TilesBuilt += record.Rebake->GetBuiltTileCount() - builtBefore;
        if (record.Rebake->IsComplete())
            record.Rebake.reset();
    }

    if (!swapped.empty())
    {
        surface.BakeVersion += swapped.size();
        for (const Pathfinding::NavTileKey& key : swapped)
            record.Stale.erase(key);
        surface.BakedWalkable = mesh->GetPolygonCount();
        surface.BakedBytes = mesh->GetTileDataBytes();
        ReplanAgentsCrossing(world, ResolveNavigationSurfaceMap(world, surface), record.MeshSettings, record.MeshChart,
                             swapped);
    }
    surface.StaleTiles = static_cast<uint32>(record.Stale.size());
    if (!record.Stale.empty() && !swapped.empty())
    {
        surface.BlockingTileX = record.Stale.begin()->X;
        surface.BlockingTileZ = record.Stale.begin()->Z;
    }
    surface.TilesDirty = static_cast<uint32>(record.DirtyQueue.size());
    surface.TilesBaking = record.Rebake ? record.Rebake->GetTileCount() - record.Rebake->GetFinishedTileCount() : 0u;
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

    Mathematics::Vector3 waitPoint;
    std::unique_ptr<Pathfinding::GridMap> grid =
        BakeNavigationGrid(settings, terrain, geometry, surface.StepHeight, surface.AgentHeight, waitPoint);
    if (!grid)
    {
        WaitForTerrainHeight(surface, terrain, waitPoint);
        record.Pending = true;
        return;
    }
    ApplyAuthoredCells(surface, entity, *grid);

    const uint32 walkable = CountWalkableCells(*grid);
    const uint64 bytes = GridDataBytes(*grid);
    const uint32 total = grid->GetCellCount();
    NavigationService::Get().ReplaceGridMap(ResolveNavigationSurfaceMap(world, surface), std::move(grid));
    surface.BakePhase = NavigationBakePhase::Baked;
    surface.MapHoldsBake = true;
    surface.BakedMode = NavigationSurfaceMode::Grid;
    surface.BakeDone = total;
    surface.BakeTotal = total;
    surface.BakedWalkable = walkable;
    surface.BakedBytes = bytes;
    ++surface.BakeVersion;
    ReplanAgentsOn(world, ResolveNavigationSurfaceMap(world, surface));
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
