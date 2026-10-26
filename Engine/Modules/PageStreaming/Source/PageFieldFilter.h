#pragma once

#include "PageStreaming/PageStoreFormat.h"

#include <algorithm>
#include <array>

namespace GameEngine::PageStreaming
{

/// The taps of a coarser sample: the finer samples 2i - 1, 2i and 2i + 1 along both axes,
/// edge-clamped, row-major.
inline constexpr uint32 kFilterTaps = 9u;
using FilterTaps = std::array<const uint8*, kFilterTaps>;

/// Makes one coarser sample of `filter` from its taps of `texelBytes` bytes each. For every filter
/// of a byte format; HeightTent filters float32 heights per row (PagePyramidBuilder).
void FilterCoarserTexel(PageFieldFilter filter, const FilterTaps& taps, uint32 texelBytes, uint8* out);

/// `index` clamped to a row of `count` samples: the edge sample stands in past either end.
inline uint32 ClampIndex(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

} // namespace GameEngine::PageStreaming
