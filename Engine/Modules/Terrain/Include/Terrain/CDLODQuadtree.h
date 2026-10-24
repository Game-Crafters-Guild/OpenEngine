#pragma once

#include "Terrain/TerrainTypes.h"

#include <cstddef>
#include <vector>

namespace GameEngine::Terrain
{

class HeightfieldData;

// Quadtree built over a heightfield for CDLOD LOD selection.
// Each node stores the min/max height of its covered region,
// enabling fast frustum culling with tight vertical bounds.
//
// The tree is a complete quadtree stored as a flat array (level-order).
// Level 0 = finest subdivision (2^(numLevels-1) nodes per axis).
// Level numLevels-1 = root (one node covering the entire terrain).
class CDLODQuadtree
{
public:
    CDLODQuadtree() = default;

    // Build the quadtree from a heightfield.
    // `numLevels` determines the depth (finest level has 2^(numLevels-1) nodes per axis).
    void Build(const HeightfieldData& heightfield, uint32 numLevels);

    // Re-scan only the finest-level nodes overlapping the sample rect
    // [minSampleX, maxSampleX] x [minSampleZ, maxSampleZ] (inclusive) from the
    // current heightfield, then re-derive every coarser level. The tree must
    // already be Build()-t. Nodes whose samples didn't change keep their
    // values, so the result is bit-identical to a full Build() whenever the
    // only samples that moved lie within the rect — the region-bake analogue of
    // Build(), avoiding an O(all-samples) rebuild per modifier edit.
    void PatchRegion(const HeightfieldData& heightfield,
                     int32 minSampleX, int32 minSampleZ,
                     int32 maxSampleX, int32 maxSampleZ);

    // Global height range read from the root node's min/max (O(1)). Valid only
    // when the finest level tiles the heightfield exactly
    // (finestNodes * samplesPerNode == dim-1 on both axes — the standard config
    // invariant): otherwise the root omits the uncovered edge samples. Returns
    // false when the tree isn't built or coverage is incomplete (a resolution
    // clamped below the LOD-implied size), so the caller falls back to a
    // full-sample scan for the exact range.
    bool TryGetGlobalHeightRange(uint32 heightfieldWidth, uint32 heightfieldHeight,
                                 float32& outMin, float32& outMax) const;

    // ---- Queries ----

    uint32 GetNumLevels() const { return m_NumLevels; }
    uint32 GetNodesPerAxisAtLevel(uint32 level) const;
    uint32 GetTotalNodeCount() const { return static_cast<uint32>(m_Nodes.size()); }

    struct Node
    {
        float32 MinHeight;
        float32 MaxHeight;
    };

    // Get a node by level and grid position within that level.
    const Node& GetNode(uint32 level, uint32 x, uint32 z) const;

    // Get the flat array index for a node.
    uint32 GetNodeIndex(uint32 level, uint32 x, uint32 z) const;

    bool IsBuilt() const { return !m_Nodes.empty(); }

private:
    // Allocate the node structure for `numLevels`, every node at min=0, max=0.
    void AllocateLevels(uint32 numLevels);

    void SetNodeMinMax(uint32 level, uint32 x, uint32 z,
                       float32 minHeight, float32 maxHeight);

    // Build every level above the finest one by merging 2x2 children:
    //   parent.MinHeight = min(children), parent.MaxHeight = max(children).
    void BuildCoarseLevelsFromChildren();

    void BuildLevel(const HeightfieldData& heightfield, uint32 level,
                    uint32 samplesPerNodeX, uint32 samplesPerNodeZ);

    uint32 m_NumLevels = 0;
    std::vector<uint32> m_LevelOffsets; // start index in m_Nodes for each level
    std::vector<uint32> m_LevelSizes;   // nodes-per-axis at each level
    std::vector<Node> m_Nodes;
};

} // namespace GameEngine::Terrain
