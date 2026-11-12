#pragma once

// CBTPoolCensus — the live leaves of the bisector pool per level, in view and off view, and the off-view leaves by
// class. Kernel_Classify counts it only on an update with CBTClassifyDesc::Census set; CBTInstance::ReadPoolCensus
// reads it.

#include "CBTTerrain/CBTLayout.h"

#include <array>
#include <cstdint>

namespace GameEngine::CBTTerrain
{

// Why an off-view leaf (its VISIBLE flag clear after Classify) is live. The classes are disjoint; a leaf
// takes the first that holds, in this order. Mirrors cbt_layout.glsl CBT_CENSUS_CLASS_*.
enum class CBTCensusClass : uint32_t
{
    MergeWanted = 0,     // Classify wants the merge (state SIMPLIFY); at rest the diamond refuses every one
    BehindEye = 1,       // the gate's box (or the triangle) lies wholly behind the eye plane
    BesideFrustum = 2,   // wholly in front of the eye, outside the gate's margin-widened frustum
    AcrossEyePlane = 3,  // the gate's box crosses the eye plane; the draw cull drops the triangle
    InMarginFrustum = 4, // the gate's margin-widened frustum holds the box; the draw cull drops the triangle, or
                         // the triangle is past a tiled terrain's edge
    BehindHorizon = 5,   // the sphere's horizon hides all three corners
};
static_assert(static_cast<uint32_t>(CBTCensusClass::BehindHorizon) + 1u == kCensusClassCount);

struct CBTPoolCensus
{
    std::array<uint32_t, kCensusLevels> InViewByLevel{};  // leaves with the VISIBLE flag, by HeapID depth
    std::array<uint32_t, kCensusLevels> OffViewByLevel{}; // the other live leaves, by HeapID depth
    std::array<uint32_t, kCensusClassCount> OffViewByClass{}; // indexed by CBTCensusClass
};

} // namespace GameEngine::CBTTerrain
