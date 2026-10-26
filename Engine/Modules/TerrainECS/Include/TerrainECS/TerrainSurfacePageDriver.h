#pragma once

#include "TerrainECS/TerrainService.h"

#include "PageStreaming/PageSampleRect.h"
#include "Types/Types.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

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

/// Keeps the surface stores of every tiled terrain current (design page-streaming 3.8). At the first
/// sight of a terrain it keeps the store of the current bake key, else makes it again. An edit bakes
/// its pages again when the edits settle. Runs on the main thread, and writes only with a cache directory.
class TerrainSurfacePageDriver
{
public:
    /// The level-0 pages a terrain bakes a frame, in every plane: a block of 2 x 2 pages. On a 4 km
    /// terrain in Release, such a frame of two planes takes about 23 ms (the median), file stalls aside.
    static constexpr std::size_t kLevel0PagesAFrame = 4;
    /// The frames with no edit and no interactive drag before the edits of a terrain bake: longer
    /// than the gap between two dabs of a stroke.
    static constexpr uint32 kSurfaceSettleFrames = 10;

    TerrainSurfacePageDriver();
    ~TerrainSurfacePageDriver();

    TerrainSurfacePageDriver(const TerrainSurfacePageDriver&) = delete;
    TerrainSurfacePageDriver& operator=(const TerrainSurfacePageDriver&) = delete;

    /// One frame for every tiled terrain of `world`: a new store bakes its pinned levels at once and
    /// kLevel0PagesAFrame pages of level 0 a frame. A terrain not seen in a frame is forgotten. `pool`
    /// runs the page bakes; null runs them here.
    void Update(ECS::World& world, TerrainService& service, TerrainModifierSystem& system,
                JobSystem::WorkStealingThreadPool* pool);

    /// The level-0 pages the tiled terrain `terrain` still bakes, in every plane.
    std::size_t PendingPages(const TiledTerrainHandle& terrain) const;

private:
    struct PlaneStore;
    struct TerrainStores;
    struct TerrainFrame;

    // One terrain of Update's frame, whose world Y is `originY`: makes, keeps or forgets its stores and
    // notes its edits. At their settle it queues their pages or makes the stores again, then it bakes
    // kLevel0PagesAFrame pages.
    void UpdateTerrain(ECS::World& world, ECS::EntityHandle entity, const Components::Terrain& terrain, float32 originY,
                       TerrainService& service, TerrainModifierSystem& system, JobSystem::WorkStealingThreadPool* pool,
                       const std::filesystem::path& directory);
    const TerrainStores* Find(const TiledTerrainHandle& terrain) const;
    // The stores of the terrain of `frame`: a store of the current key is kept, else made again. With
    // `rebuild`, every store is made again.
    static std::unique_ptr<TerrainStores> OpenStores(const TerrainFrame& frame, bool rebuild);
    // Adds the edits of this frame to the unsettled edits of `stores`. True when the frame edited.
    static bool NoteEdits(TerrainStores& stores, const TiledTerrainData& tiled, const TerrainColorGroundWrite& write);
    // Queues next the level-0 pages that read `changed`.
    static void QueueEdit(TerrainStores& stores, const PageStreaming::PageSampleRect& changed);
    // Bakes the next kLevel0PagesAFrame pending pages in every plane.
    static void BakePending(TerrainStores& stores);

    std::vector<std::unique_ptr<TerrainStores>> m_Terrains;
};

} // namespace GameEngine::TerrainECS
