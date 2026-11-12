#pragma once

#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <span>

namespace GameEngine::TerrainECS
{

struct PageLevelView;

/// A forward shorter than this in a camera's ground plane has no bearing: the wedge is the disc.
inline constexpr float32 kPageWedgeMinForward = 1e-3f;

/// The cache slots a wedge request fills over `levels` streamed levels at `view`: a camera's half field
/// `horizontalHalfFov` plus the margin and the foot, each level's reach at most `horizonMeters`, for a
/// level-0 texel of `level0Texel` meters. The pinned quarter comes on top; the count fills whole rows.
uint32 WedgePageCacheSlots(const PageLevelView& view, float32 horizontalHalfFov, float32 level0Texel,
                           float32 horizonMeters, uint32 levels);

/// True when a convex footprint meets a camera's wedge. `corners` are its ground corners (x, z) from
/// the camera's foot, which lies outside it; `forward` is the unit ground forward and `cosReach` the
/// cosine of the half field plus the margin.
bool FootprintMeetsWedge(std::span<const Mathematics::Vector2> corners, const Mathematics::Vector2& forward,
                         float32 cosReach);

} // namespace GameEngine::TerrainECS
