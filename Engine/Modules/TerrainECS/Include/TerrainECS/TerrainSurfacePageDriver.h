#pragma once

#include "TerrainECS/TerrainService.h"

#include "PageStreaming/PageSampleRect.h"
#include "Types/Types.h"

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::PageStreaming
{
class PageStoreAccess;
enum class PageFieldKind : uint8;
struct PageStoreHeader;
struct PageStoreLevel;
} // namespace GameEngine::PageStreaming

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
} // namespace GameEngine::ECS

namespace GameEngine::Components
{
struct Terrain;
}

namespace GameEngine::TerrainECS
{

class TerrainSurfacePageBaker;
class TerrainModifierSystem;
struct TerrainColorGroundWrite;

/// Keeps the surface stores of every tiled terrain current (design page-streaming 3.8): it keeps the
/// store of the current bake key, else makes it again, and bakes the pages of an edit at its settle.
/// One job a terrain bakes from a snapshot; the main thread never waits for it. Needs a cache directory.
class TerrainSurfacePageDriver
{
public:
    /// The level-0 pages a job bakes, in every plane: a block of 4 x 4 pages. On the 8 km terrain in
    /// Release, a job of two planes takes about 35 ms on the workers.
    static constexpr std::size_t kLevel0PagesAJob = 16;
    /// The frames with no edit and no interactive drag before the edits of a terrain bake: longer
    /// than the gap between two dabs of a stroke.
    static constexpr uint32 kSurfaceSettleFrames = 10;

    TerrainSurfacePageDriver();
    ~TerrainSurfacePageDriver();

    TerrainSurfacePageDriver(const TerrainSurfacePageDriver&) = delete;
    TerrainSurfacePageDriver& operator=(const TerrainSurfacePageDriver&) = delete;

    /// One frame for every tiled terrain of `world`: takes the result of a finished job, then starts
    /// the next job: a new store's pinned levels, else kLevel0PagesAJob level-0 pages. A terrain not
    /// seen in a frame is forgotten once its job ends. `pool` runs the jobs; null runs each job here.
    void Update(ECS::World& world, TerrainService& service, TerrainModifierSystem& system,
                JobSystem::WorkStealingThreadPool* pool);

    /// The level-0 pages the tiled terrain `terrain` still bakes, in every plane: the queued pages
    /// and the pages of its job. A page queued again while its job bakes it counts twice.
    std::size_t PendingPages(const TiledTerrainHandle& terrain) const;

    /// The `plane` store of the tiled terrain `terrain`, readable while it bakes: the source of the
    /// plane's page stream (PageSource::Access). The same object over every run and every new key
    /// on the same lattice. Null when the terrain has no `plane` store.
    std::shared_ptr<const PageStreaming::PageStoreAccess> SurfaceStore(const TiledTerrainHandle& terrain,
                                                                     PageStreaming::PageFieldKind plane) const;

private:
    struct PlaneStore;
    struct TerrainStores;
    struct TerrainFrame;
    struct BakeJob;

    // One terrain of Update's frame, whose world Y is `originY`: makes, keeps or forgets its stores and
    // notes its edits. At their settle it queues their pages or makes the stores again, then it bakes
    // kLevel0PagesAFrame pages.
    void UpdateTerrain(ECS::World& world, ECS::EntityHandle entity, const Components::Terrain& terrain, float32 originY,
                       TerrainService& service, TerrainModifierSystem& system, JobSystem::WorkStealingThreadPool* pool,
                       const std::filesystem::path& directory);
    const TerrainStores* Find(const TiledTerrainHandle& terrain) const;
    // The stores of the terrain of `frame`, and the job that opens them: a store of the current key is
    // kept, else made again. With `rebuild`, every store is made again. The job first publishes the
    // `retired` stores the terrain had.
    static std::unique_ptr<TerrainStores> OpenStores(const TerrainFrame& frame, bool rebuild,
                                                     std::vector<std::shared_ptr<PlaneStore>> retired);
    // Adds the edits of this frame to the unsettled edits of `stores`. True when the frame edited.
    static bool NoteEdits(TerrainStores& stores, const TiledTerrainData& tiled, const TerrainColorGroundWrite& write);
    // Queues next the level-0 pages that read `changed`.
    static void QueueEdit(TerrainStores& stores, const PageStreaming::PageSampleRect& changed);
    // Starts `job` on `pool` for `stores`, or runs it here when `pool` is null.
    static void Start(TerrainStores& stores, std::shared_ptr<BakeJob> job, JobSystem::WorkStealingThreadPool* pool);
    // Takes the result of the job of `stores` once it ended. True when `stores` has no job.
    static bool Poll(TerrainStores& stores);
    // The store access of the `plane` store in `retired` when it is on the lattice of `shape`, else null.
    static std::shared_ptr<PageStreaming::PageStoreAccess>
    KeptAccess(std::span<const std::shared_ptr<PlaneStore>> retired, PageStreaming::PageFieldKind plane,
               const PageStreaming::PageStoreHeader& shape);
    // True when the store of `store` is on disk under its key, with its samples cache for a Normal store.
    static bool HoldsCurrentStore(const PlaneStore& store);
    // The open step of `job`: closes the retired accesses no plane keeps, deletes the stores of the
    // planes nothing writes, keeps each current store and notes the level-0 pages it lacks, and makes
    // the others with their pinned levels.
    static void OpenJobStores(BakeJob& job, const PageStreaming::PageStoreLevel& level0);
    // The body of `job`, on a pool worker or here: the open step, then the level-0 pages of every plane.
    static void RunJob(BakeJob& job, const PageStreaming::PageStoreLevel& level0);
    // Starts the job of the next kLevel0PagesAJob queued pages, when `stores` has no job. The job
    // that empties the queue publishes the stores.
    static void BakePending(TerrainStores& stores, const TerrainFrame& frame);
    // Starts the job that publishes the open stores of `stores`, which has no job.
    static void Close(TerrainStores& stores, JobSystem::WorkStealingThreadPool* pool);

    std::vector<std::unique_ptr<TerrainStores>> m_Terrains;
};

} // namespace GameEngine::TerrainECS
