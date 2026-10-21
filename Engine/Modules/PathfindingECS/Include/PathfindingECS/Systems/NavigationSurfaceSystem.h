#pragma once
#include "ECS/Systems.h"
#include "Assets/FileWatchingService.h"
#include "AssetCore/GUID.h"
#include "Pathfinding/PathfindingTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::Components
{
struct NavigationSurface;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::Pathfinding
{
class GridMap;
} // namespace GameEngine::Pathfinding

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::PathfindingECS
{

struct NavigationBounds;
struct NavigationStaticGeometry;
struct NavigationTerrainSource;
class NavigationMeshBake;

// Bakes every NavigationSurface: when it first appears (or after its map was released) and on
// RequestBake. A mesh bakes as Detour tiles built on the job system, at most
// kTileSnapshotsPerUpdate tiles started per update; a grid bakes in one update. Both read the
// terrain's heightfield, the collider shapes of static bodies and mesh renderers, never a physics
// world, so a surface bakes in edit mode, in Play and in a game alike. Also stamps
// NavigationObstacle footprints into grid surfaces every update, and publishes each surface's bake
// state into the component for the inspector.
class NavigationSurfaceSystem : public ECS::ISystem
{
public:
    // Mesh tiles a bake snapshots on the main thread per update. A count, not a time, so the tiles a
    // frame starts do not depend on the machine.
    static constexpr uint32 kTileSnapshotsPerUpdate = 64;

    NavigationSurfaceSystem();
    ~NavigationSurfaceSystem() override;

    const char* GetName() const override { return "NavigationSurfaceSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // What this system knows about one surface: the bake in progress, and what the map holds.
    struct SurfaceRecord;

    // `placed` is false while the surface's position is not known yet; its bake waits for it.
    void UpdateSurface(ECS::World& world, ECS::EntityHandle entity, Components::NavigationSurface& surface,
                       const Components::WorldTransform* transform, bool placed,
                       JobSystem::WorkStealingThreadPool* pool);
    void StartBake(ECS::World& world, ECS::EntityHandle entity, Components::NavigationSurface& surface,
                   const Components::WorldTransform* transform, SurfaceRecord& record);
    // The first model the bake reads that is still loading, or a null GUID. Requests the load of
    // every model that is not resident; a model whose load ended without it is left out of the
    // bake, with one warning.
    GUID FirstLoadingModel(ECS::World& world, ECS::EntityHandle entity, SurfaceRecord& record);
    void StartMeshBake(ECS::World& world, ECS::EntityHandle entity, Components::NavigationSurface& surface,
                       const NavigationTerrainSource& terrain,
                       std::shared_ptr<const NavigationStaticGeometry> geometry, const NavigationBounds& bounds,
                       SurfaceRecord& record);
    void BakeGrid(ECS::World& world, ECS::EntityHandle entity, Components::NavigationSurface& surface,
                  const NavigationTerrainSource& terrain, const NavigationStaticGeometry& geometry,
                  const NavigationBounds& bounds, SurfaceRecord& record);
    void StepMeshBake(ECS::World& world, ECS::EntityHandle entity, Components::NavigationSurface& surface,
                      JobSystem::WorkStealingThreadPool* pool, SurfaceRecord& record);
    // Releases the maps m_Unbound collected that no surface in `world` names.
    void ReleaseUnboundMaps(ECS::World& world);
    void ApplyAuthoredCells(const Components::NavigationSurface& surface, ECS::EntityHandle entity,
                            Pathfinding::GridMap& grid);

    // Rasterizes NavigationObstacle footprints into every grid surface's dynamic-blocked cells
    // (clear + reapply). Lives here, with the other map writes behind this system's
    // HasActivePathJobs gate: dynamic-blocked is an unsynchronized field that path solves read.
    void ApplyDynamicObstacles(ECS::World& world);

    // Keeps the .navgrid watch pointed at the current project asset root, re-subscribing when the
    // root moves. Called every update; gated on the asset manager's source-set version so the common
    // frame costs one atomic load. Does nothing while watching is unarmed or no project is mounted,
    // and retries on the next mount or rebind.
    void UpdateAuthoredCellsWatch();

    // Consecutive frames the HasActivePathJobs gate has deferred map mutation. Triggers persist, so
    // deferral delays work rather than losing it — but a long streak means surfaces are not updating
    // and is logged once per streak.
    uint32 m_GateDeferredFrames = 0;
    uint64 m_UpdateIndex = 0;

    std::unordered_map<ECS::EntityHandle, std::unique_ptr<SurfaceRecord>, ECS::EntityHandleHash> m_Surfaces;
    // Maps a surface was bound to and no longer is (undo or redo restored bytes naming another
    // map). Released at the end of the update unless some surface in the world still names one.
    std::vector<Pathfinding::NavMapHandle> m_Unbound;

    // Shared by the main thread, asset load callbacks and file-watcher callbacks. Held by shared_ptr
    // and captured by value into every callback, so it outlives the system: an async load can
    // complete after ~NavigationSurfaceSystem.
    struct AsyncState
    {
        std::mutex Mutex;
        // Grid surfaces' authored cells changed on disk or finished loading: those grids re-bake.
        bool AuthoredCellsChanged = false;
        // .navgrid assets with a load in flight.
        std::unordered_set<GUID> Loading;
    };
    std::shared_ptr<AsyncState> m_AsyncState;

    // File watcher subscription for .navgrid edits, and the asset root it covers (empty while
    // unsubscribed). Source-set version it was built for: mounting or rebinding a source is the only
    // thing that can move the project root, and bumps it. Version 0 means no source is mounted yet,
    // so starting there costs no initial re-evaluation.
    uint64 m_WatchedSourceSetVersion = 0;
    std::filesystem::path m_WatchedAssetRoot;
    std::optional<FileWatchSubscription> m_NavGridWatchSub;
};

} // namespace GameEngine::PathfindingECS
