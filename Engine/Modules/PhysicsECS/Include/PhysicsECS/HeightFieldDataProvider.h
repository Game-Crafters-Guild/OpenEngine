#pragma once

#include "Types/Types.h"

#include <vector>

namespace GameEngine::Components
{
struct HeightFieldColliderShape;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::Mathematics
{
struct AABB;
} // namespace GameEngine::Mathematics

namespace GameEngine::Physics
{
class PhysicsWorld;
} // namespace GameEngine::Physics

namespace GameEngine::PhysicsECS
{

// Result returned by a HeightFieldDataProvider when resolving height data
// for a HeightFieldColliderShape. The provider fills this struct; the caller
// (PhysicsInitSystem) uses it to build a HeightFieldShapeDef or to update a
// region of an already-built shape in place.
struct HeightFieldData
{
    const float32* samples = nullptr; // Non-owning; valid until the shape is created. Physics::kHeightFieldNoCollision marks a hole.
    uint32 sampleCount = 0;          // Width/height of the square sample grid.
    uint64 version = 0;              // Data version for change detection.

    // Union of the sample-space regions dirtied after the caller-supplied
    // sinceVersion cursor, clamped to the grid. Max bounds are exclusive.
    // When false, the caller must treat the whole grid as dirty.
    bool hasRegion = false;
    int32 regionMinX = 0;
    int32 regionMinZ = 0;
    int32 regionMaxX = 0;
    int32 regionMaxZ = 0;
};

// Callback type for resolving heightfield data from a handle.
// PhysicsInitSystem calls this during the gather phase for each
// HeightFieldColliderShape it encounters. sinceVersion is the consumer's
// cursor (the version its physics shape was last built/updated from); the
// provider reports the dirty region accumulated after it.
//
// TerrainECS registers its provider at engine startup so that PhysicsECS
// can build heightfield shapes without depending on the TerrainECS module.
using HeightFieldDataProvider = HeightFieldData(*)(uint32 handle, uint32 generation, uint64 sinceVersion);

// Register / query the global heightfield data provider.
void SetHeightFieldDataProvider(HeightFieldDataProvider provider);
HeightFieldDataProvider GetHeightFieldDataProvider();

/// The world-space box of sample region [minX, maxX] x [minZ, maxZ] of a heightfield collider of
/// `sampleCount` samples a side, between raw heights `minRawHeight` and `maxRawHeight`: the region's
/// local box through the body's full world matrix, padded so a body resting on the region overlaps it.
/// The box to wake sleeping bodies in when the ground under them changes or goes.
Mathematics::AABB HeightFieldRegionWorldBox(const Components::WorldTransform& wt,
                                            const Components::HeightFieldColliderShape& shape, uint32 sampleCount,
                                            int32 minX, int32 minZ, int32 maxX, int32 maxZ, float32 minRawHeight,
                                            float32 maxRawHeight);

/// Wakes the dynamic bodies in `wakeBox`. A body whose bottom is under the heightfield's surface below its
/// center is buried (a heightfield has no caves): it moves up along the heightfield's up axis until its
/// bottom is the contact distance over the surface. Call it when this heightfield replaces the ground.
void SettleBodiesOnHeightField(Physics::PhysicsWorld& physicsWorld, const Components::WorldTransform& wt,
                               const Components::HeightFieldColliderShape& shape, const Mathematics::AABB& wakeBox);

} // namespace GameEngine::PhysicsECS
