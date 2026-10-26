#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Runtime tag placed by TerrainPhysicsSystem on each page heightfield collider entity of a paged
// terrain (design page-streaming D3): one Jolt body per finest page inside a CPU window. It records
// the terrain and page it serves and the page physics handle to release when the page leaves every
// window or the collider is torn down. Not authored, not serialized: a runtime provisioning link.
struct TerrainPageCollider
{
    // The terrain's tiled handle (Terrain::TiledTerrainHandle, ::TiledTerrainGeneration).
    uint32 TiledIndex = 0;
    uint32 TiledGeneration = 0;
    // The level-0 page.
    uint32 PageX = 0;
    uint32 PageZ = 0;

    // HeightFieldColliderShape.dataHandle value (already OR'd with
    // TerrainService::kPagePhysicsHandleBit) and its generation; passed to
    // TerrainService::ReleasePagePhysicsHandle on teardown.
    uint32 PhysicsHandleIndex = 0;
    uint32 PhysicsHandleGeneration = 0;
};

static_assert(std::is_trivially_copyable_v<TerrainPageCollider>,
              "TerrainPageCollider must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainPageCollider>,
              "TerrainPageCollider must be standard layout for ECS storage");

} // namespace GameEngine::Components
