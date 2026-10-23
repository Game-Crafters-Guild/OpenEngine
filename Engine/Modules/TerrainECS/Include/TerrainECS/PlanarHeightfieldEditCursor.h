#pragma once

#include "Mathematics/Rect.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "Types/Types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine::TerrainECS
{

// One consumer's place in the edits of the planar terrain a PlanarHeightQuery reads: the
// heightfield's version (one heightfield), or the tiled terrain's revision and each loaded tile's
// heightfield version. The edits themselves are read from the terrain's dirty-region logs, which
// nobody clears, so any number of consumers keep a cursor each. Main thread only, like the
// heightfields.
class PlanarHeightfieldEditCursor
{
  public:
    // Starts the cursor at the terrain's current state: edits before it are not reported.
    void Reset(const PlanarHeightQuery& query);

    // Appends to `out` the rectangles the terrain's heightfields changed in since the cursor, in
    // world metres with X along world X and Y along world Z, each covering every lattice quad that
    // touches a changed sample, and moves the cursor past them. A tile of a tiled terrain the cursor
    // has not seen is recorded, not reported: it streamed in; a tile that unloaded is forgotten. A
    // query for another terrain than the cursor's starts the cursor there.
    void CollectEdits(const PlanarHeightQuery& query, std::vector<Mathematics::Rect>& out);

  private:
    const void* m_Terrain = nullptr;
    uint64 m_Version = 0;
    // Tiled: each loaded tile's heightfield version, by its packed coordinate.
    std::unordered_map<uint64, uint64> m_TileVersions;
};

} // namespace GameEngine::TerrainECS
