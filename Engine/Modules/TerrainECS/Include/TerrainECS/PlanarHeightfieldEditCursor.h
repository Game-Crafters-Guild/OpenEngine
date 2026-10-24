#pragma once

#include "Mathematics/Rect.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::TerrainECS
{

// One consumer's place in the edits of the planar terrain a SurfaceHeightQuery reads: a whole-map
// terrain's heightfield version, or a paged terrain's page source and modifier overlay version
// (SurfaceHeightEdits). The edits are read from the terrain's own records, which nobody clears, so
// any number of consumers keep a cursor each. A page landing on the CPU is not an edit: it holds the
// heights its source always had. Main thread only, like the heights.
class PlanarHeightfieldEditCursor
{
  public:
    // Starts the cursor at the terrain's current state: edits before it are not reported.
    void Reset(const SurfaceHeightQuery& query);

    // Appends to `out` the rectangles the terrain's heights changed in since the cursor, in world
    // meters with X along world X and Y along world Z, each covering every lattice quad that touches
    // a changed sample, and moves the cursor past them. A paged terrain whose source changed, or
    // whose overlay changed more than once since the cursor, reports its whole footprint. A query for
    // another terrain than the cursor's, or a paged terrain receiving its first source, starts the
    // cursor there.
    void CollectEdits(const SurfaceHeightQuery& query, std::vector<Mathematics::Rect>& out);

  private:
    uint64 m_WholeMap = 0; // the whole-map terrain (SurfaceHeightQuery::WholeMapIdentity), 0 on a paged terrain
    uint64 m_Version = 0; // the whole map's heightfield version, or the paged terrain's overlay version
    uint64 m_Source = 0;  // the paged terrain's page source
};

} // namespace GameEngine::TerrainECS
