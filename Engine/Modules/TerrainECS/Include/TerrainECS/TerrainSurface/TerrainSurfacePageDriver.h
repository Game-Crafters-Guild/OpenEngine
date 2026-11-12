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

/// Keeps the surface stores of every tiled terrain current (design page-streaming 3.8) and bakes an edit at its
/// settle in a job; the main thread never waits. A terrain whose scene is not saved bakes into the session folder
/// (<cache>/Session/<GUID>): deleted with the terrain, at the save of its scene and with the driver. Needs a cache directory.
class TerrainSurfacePageDriver
{
public:
    /// The pages a job bakes, in every plane: a block of 4 x 4 pages of one level, or 16 edit pages. On the
    /// 8 km terrain in Release, a job of two planes takes about 35 ms on the workers.
    static constexpr std::size_t kPagesAJob = 16;
    /// The frames with no edit and no interactive drag before the edits of a terrain bake: longer
    /// than the gap between two dabs of a stroke.
    static constexpr uint32 kSurfaceSettleFrames = 10;

    TerrainSurfacePageDriver();
    ~TerrainSurfacePageDriver();

    TerrainSurfacePageDriver(const TerrainSurfacePageDriver&) = delete;
    TerrainSurfacePageDriver& operator=(const TerrainSurfacePageDriver&) = delete;

    /// One frame for every tiled terrain of `world`: takes the result of a finished job, then starts
    /// the next job: a new store's pinned levels, else kPagesAJob pages. A terrain not
    /// seen in a frame is forgotten once its job ends. `pool` runs the jobs; null runs each job here.
    void Update(ECS::World& world, TerrainService& service, TerrainModifierSystem& system,
                JobSystem::WorkStealingThreadPool* pool);

    /// The pages the tiled terrain `terrain` still bakes, in every plane: the queued edit pages, the wanted
    /// pages a plane lacks and the pages of its job. A page queued again while its job bakes it counts twice.
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
    struct Session;

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
    // The plane store of `plane` of the terrain of `frame` under `key`, on the access of `retired` when
    // one is on its lattice.
    static std::shared_ptr<PlaneStore> MakePlaneStore(const TerrainFrame& frame, const TerrainStores& stores,
                                                      PageStreaming::PageFieldKind plane, uint64 key,
                                                      std::span<const std::shared_ptr<PlaneStore>> retired);
    // Adds the edits of this frame to the unsettled edits of `stores`, and takes the grass edits of
    // `tiled`. True when the frame edited.
    static bool NoteEdits(TerrainStores& stores, TiledTerrainData& tiled, const TerrainColorGroundWrite& write);
    // Queues next, for each plane of the mask `planes`, the present pages of each level of that plane that
    // read `changed`.
    static void QueueEdit(TerrainStores& stores, const PageStreaming::PageSampleRect& changed, uint8 planes);
    // Starts the run of `stores` for its log line: the count, the planes and the time since now.
    static void StartRun(TerrainStores& stores);
    // Makes the GrassControl store of `stores` when `key` is not 0, else deletes it, with one job that
    // bakes only that plane. `stores` has no job.
    static void ChangeGrassStore(TerrainStores& stores, const TerrainFrame& frame, uint64 key);
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
    // planes nothing writes, keeps each current store, makes the others with their pinned levels, and
    // reads the pages each store holds.
    static void OpenJobStores(BakeJob& job);
    // The body of `job`, on a pool worker or here: the open step, then the pages of each plane.
    static void RunJob(BakeJob& job);
    // When `stores` has no job: queues the pages the surface stream wants that a plane lacks, then starts
    // the job of the next kPagesAJob queued pages. The job that empties the queue publishes the stores; when
    // a Want empties it after a job that left them open, a job of no page publishes them.
    static void BakePending(TerrainStores& stores, const TerrainFrame& frame);
    // Starts the job of no page that publishes the open stores of `stores`, which has no job.
    static void StartFinish(TerrainStores& stores, JobSystem::WorkStealingThreadPool* pool);
    // Starts the job that publishes the open stores of `stores`, which has no job.
    static void Close(TerrainStores& stores, JobSystem::WorkStealingThreadPool* pool);
    // Sets the surface page source of the terrain of `stores`: its planes' store accesses, or none when
    // the frame did not see the terrain.
    static void PublishSource(TerrainService& service, const TerrainStores& stores);
    // The session of the cache `cache`, made at the first terrain whose scene is not saved; null when
    // its lock is refused. A refused session stays refused until the cache changes.
    const Session* OpenSession(const std::filesystem::path& cache);
    // Deletes the session folder and lets its lock go, when it holds the lock. A store that a job still
    // writes can stay.
    void EndSession();
    // Closes and deletes the stores of `stores` when they are in the session.
    void DeleteSessionStores(const TerrainStores& stores) const;
    // Closes the store accesses of each dropped terrain whose job ended, and forgets the terrain.
    void CloseDroppedStores();

    std::vector<std::unique_ptr<TerrainStores>> m_Terrains;
    // The terrains that a cleared cache location dropped while a job ran. The job's Finish opens a reader
    // in each access, so the accesses close when the job ends; the terrain opens again only then.
    std::vector<std::unique_ptr<TerrainStores>> m_Dropped;
    std::unique_ptr<Session> m_Session;
    // The cache whose orphan sessions this driver deleted.
    std::filesystem::path m_SweptCache;
};

} // namespace GameEngine::TerrainECS
