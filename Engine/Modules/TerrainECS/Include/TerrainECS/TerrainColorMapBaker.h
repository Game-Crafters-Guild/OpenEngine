#pragma once

#include "ECS/ECS.h"
#include "TerrainECS/TerrainColorMap.h"
#include "Types/Types.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::TerrainECS
{

class TerrainService;
struct ResolvedModifier;
struct TerrainData;

// Keeps every planar terrain's colour map in step with its writers: the splat (through the
// splat-change markers every splat writer sets), the grass's ground write, the grass control
// field's region scales and the colour zones. Run by the modifier system after its bakes.
//
// An untiled terrain composes one map on its sample lattice. A tiled terrain composes one map
// per streamed tile on the tile's lattice; an atlas-backed one adds a coarse map over the whole
// terrain, composed on the coarse splat (whose unstreamed texels read splat layer 0), so the
// written soil continues past the resident window. A terrain whose unified textures fit composes
// a unified map assembled from the tiles.
//
// While an interactive edit runs (a gizmo or inspector drag) the maps wait and recompose once it
// settles, as the splat's whole-terrain renormalize does.
class TerrainColorMapBaker
{
public:
    // The ground write the terrain entity's grass asks for: on when the terrain is planar and its
    // TerrainGrass is present, enabled, places blades and has a Darken Ground above 0.
    static TerrainColorGroundWrite ResolveGroundWrite(const ECS::World& world, ECS::EntityHandle terrainEntity);

    // The compose inputs of an untiled terrain whose lattice corner sits at (originX, originZ).
    static TerrainColorComposeInputs SingleTerrainInputs(const TerrainData& data, float32 originX, float32 originZ,
                                                         const TerrainColorGroundWrite& write,
                                                         std::span<const ResolvedModifier> zones);

    // What a map composed from these inputs depends on besides its splat: a change of any of them
    // recomposes the whole map. `zonesStamp` folds every colour zone's state.
    static uint64 InputsStamp(const TerrainColorComposeInputs& inputs, uint64 grassFieldVersion, uint64 zonesStamp);

    // Recompose an untiled terrain's map where its inputs or its splat changed, and consume the
    // terrain's splat-change markers.
    static void UpdateSingle(TerrainData& data, const TerrainColorComposeInputs& inputs, uint64 stamp);

    void Update(ECS::World& world, TerrainService& service, std::span<const ResolvedModifier> zones,
                uint64 zonesStamp);

private:
    friend struct TerrainColorMapBakerTestAccess;

    struct TiledState
    {
        uint64 Revision = ~0ull;
        uint64 Stamp = 0;
    };
    std::unordered_map<uint64, TiledState> m_Tiled;
    std::vector<uint8> m_CoarseSplat;
};

} // namespace GameEngine::TerrainECS
