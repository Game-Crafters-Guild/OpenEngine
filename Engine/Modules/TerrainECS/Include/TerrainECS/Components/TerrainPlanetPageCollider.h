#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Runtime tag placed by TerrainPhysicsSystem on each page heightfield collider entity of a streamed
// planet (design page-streaming-planets-r4-colliders): one Jolt body per level-0 page of the planet's
// CPU windows, placed in the page's chart frame. It records the planet, the page, the stream the
// samples came from and the page physics handle to release when the page leaves every window or the
// collider is torn down. Not authored, not serialized: a runtime provisioning link.
struct TerrainPlanetPageCollider
{
    // The planet's terrain entity (index and version as raw values, so the component stays standard
    // layout).
    uint32 TerrainEntityIndex = 0;
    uint32 TerrainEntityVersion = 0;
    // The level-0 page: cube face (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) and page coordinates.
    uint32 Face = 0;
    uint32 PageX = 0;
    uint32 PageZ = 0;

    // HeightFieldColliderShape.dataHandle value (already OR'd with
    // TerrainService::kPagePhysicsHandleBit) and its generation; passed to
    // TerrainService::ReleasePagePhysicsHandle on teardown.
    uint32 PhysicsHandleIndex = 0;
    uint32 PhysicsHandleGeneration = 0;

    // The planet stream's identity the samples were read from: a restart gives the stream a new one,
    // and the body stays until the new stream's body for the same page is built.
    uint64 Epoch = 0;
    // The planet page layer version the samples include.
    uint64 LayerVersion = 0;
};

static_assert(std::is_trivially_copyable_v<TerrainPlanetPageCollider>,
              "TerrainPlanetPageCollider must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainPlanetPageCollider>,
              "TerrainPlanetPageCollider must be standard layout for ECS storage");

} // namespace GameEngine::Components
