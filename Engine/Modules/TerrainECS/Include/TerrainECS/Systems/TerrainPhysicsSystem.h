#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

#include <memory>
#include <vector>

namespace GameEngine::Components
{
struct HeightFieldColliderShape;
struct Terrain;
} // namespace GameEngine::Components

namespace GameEngine::TerrainECS
{

class TerrainService;
struct PlanetPageFill;
struct PlanetColliderScratch;

// A planar terrain's collider describes it when it reads the terrain's current
// data handle and was sized from the terrain's current extent and height scale.
bool ColliderMatchesTerrain(const Components::HeightFieldColliderShape& shape, const Components::Terrain& terrain);

// Provisions HeightFieldColliderShape + PhysicsBody + PhysicsCollider on terrain
// entities that have height data ready (a tiled terrain on its window pages, a single
// one on itself), and keeps a planar terrain's shape on the
// terrain's current data handle, extent and height scale. The actual Jolt body
// creation is handled by PhysicsInitSystem via its heightfield gather block.
class TerrainPhysicsSystem : public ECS::ISystem
{
public:
    TerrainPhysicsSystem();
    ~TerrainPhysicsSystem() override;

    const char* GetName() const override { return "TerrainPhysicsSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Provisions a streamed planet's page bodies (design page-streaming-planets-r4-colliders 5): one per
    // level-0 page of its CPU windows, filled on the job pool and placed in the page's chart frame. A
    // body from an older stream stays until the new stream's body for its page is built.
    void UpdatePlanetColliders(ECS::World& world, TerrainService& service);

    // Provisions a tiled terrain's colliders: one per finest page in its CPU windows (the cameras' and
    // the dynamic bodies', independent of the view), torn down when the page leaves every window. A
    // terrain whose pages have no source yet (its height store still cooking) gets none.
    void UpdatePageColliders(ECS::World& world, TerrainService& service);

    // Removes the collider of a terrain that lost its planar data with no
    // planar replacement: the Terrain component was removed, or the terrain
    // became tiled or spherical. Its data is gone, so the collider could never
    // be rebuilt and would leave stale collision behind.
    void RemoveOrphanedPlanarColliders(ECS::World& world);

    // Entities carrying a planar terrain collider, for RemoveOrphanedPlanarColliders.
    // Scoped to one world lifetime; cleared when the world or its reset
    // generation changes.
    std::vector<ECS::EntityHandle> m_PlanarColliders;
    uint64 m_TrackedWorld = 0;
    uint64 m_TrackedReset = 0;

    // Latched once a second enabled spherical terrain is seen (only the first gets page bodies), so the
    // warning is logged once per system.
    bool m_WarnedDuplicatePlanetClaim = false;

    // The planet page body fills on the job pool, at most one per page. Cleared when the world or its
    // reset generation changes; a running job keeps its own fill alive.
    std::vector<std::shared_ptr<PlanetPageFill>> m_PlanetFills;
    // The planet pass's per-frame containers.
    std::unique_ptr<PlanetColliderScratch> m_PlanetScratch;
};

} // namespace GameEngine::TerrainECS
