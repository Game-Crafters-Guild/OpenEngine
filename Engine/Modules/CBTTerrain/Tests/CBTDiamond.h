#pragma once

// The LEB diamond of a planar leaf, from its corners' UVs alone, for the tests of the planar split's
// merge (cbt_kernels.comp CBT_PlanarClassify): the leaf's parent P, P's twin Q across its hypotenuse,
// and whether all four children of the diamond are leaves of a tree, the one state in which the
// diamond can merge.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <set>

#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTTreeAudit.h"

namespace GameEngine::CBTTerrain::Test
{

using DiamondUv = std::array<float, 2>;
using DiamondFacet = std::array<DiamondUv, 3>;

// A facet's three corners as an unordered key (exact: LEB UVs are dyadic).
using DiamondKey = std::array<uint64_t, 3>;

inline DiamondKey KeyOf(const DiamondFacet& f)
{
    DiamondKey k{};
    for (uint32_t i = 0; i < 3u; ++i)
        k[i] = static_cast<uint64_t>(std::bit_cast<uint32_t>(f[i][0])) |
               (static_cast<uint64_t>(std::bit_cast<uint32_t>(f[i][1])) << 32);
    std::sort(k.begin(), k.end());
    return k;
}

inline DiamondFacet LeafCorners(const CBTVertexData& vd)
{
    return {DiamondUv{vd.Corner0[3], vd.Meta[0]}, DiamondUv{vd.Corner1[3], vd.Meta[1]},
            DiamondUv{vd.Corner2[3], vd.Meta[2]}};
}

// The diamond of the leaf with corners `f` and heap id parity `odd`: P = (P0, P1, P2) and, when P's
// hypotenuse is interior, Q's apex. The even child is (P2, mid, P1), the odd (P1, mid, P0).
struct Diamond
{
    DiamondFacet Parent;
    DiamondUv TwinApex;
    bool HasTwin = false;
};

inline Diamond DiamondOf(const DiamondFacet& f, bool odd)
{
    const DiamondUv& u0 = f[0];
    const DiamondUv& u1 = f[1];
    const DiamondUv& u2 = f[2];
    Diamond d;
    d.Parent[0] = odd ? u2 : DiamondUv{2.0f * u1[0] - u0[0], 2.0f * u1[1] - u0[1]};
    d.Parent[1] = odd ? u0 : u2;
    d.Parent[2] = odd ? DiamondUv{2.0f * u1[0] - u2[0], 2.0f * u1[1] - u2[1]} : u0;
    const DiamondFacet& p = d.Parent;
    d.TwinApex = {p[0][0] + p[2][0] - p[1][0], p[0][1] + p[2][1] - p[1][1]};
    d.HasTwin = d.TwinApex[0] >= 0.0f && d.TwinApex[1] >= 0.0f && d.TwinApex[0] <= 1.0f && d.TwinApex[1] <= 1.0f;
    return d;
}

// The live leaves of a snapshot taken with its corners, by their corners.
inline std::set<DiamondKey> LeafKeys(const CBTTreeSnapshot& snapshot)
{
    std::set<DiamondKey> keys;
    for (uint32_t slot = 0; slot < snapshot.PoolSize; ++slot)
        if (snapshot.HeapIds[slot] != 0u)
            keys.insert(KeyOf(LeafCorners(snapshot.Vertices[slot])));
    return keys;
}

// Whether every child of the diamond is a leaf: P's two children and, with a twin, Q's two.
inline bool DiamondChildrenAreLeaves(const std::set<DiamondKey>& leaves, const Diamond& d)
{
    const DiamondFacet& p = d.Parent;
    const DiamondUv mid = {0.5f * (p[0][0] + p[2][0]), 0.5f * (p[0][1] + p[2][1])};
    if (!leaves.contains(KeyOf({p[2], mid, p[1]})) || !leaves.contains(KeyOf({p[1], mid, p[0]})))
        return false;
    return !d.HasTwin ||
           (leaves.contains(KeyOf({p[0], mid, d.TwinApex})) && leaves.contains(KeyOf({d.TwinApex, mid, p[2]})));
}

} // namespace GameEngine::CBTTerrain::Test
