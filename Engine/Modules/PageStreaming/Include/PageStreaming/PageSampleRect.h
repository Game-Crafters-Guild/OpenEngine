#pragma once

#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::PageStreaming
{

/// An inclusive rect of one level's samples.
struct PageSampleRect
{
    uint32 MinX = 0;
    uint32 MinZ = 0;
    uint32 MaxX = 0;
    uint32 MaxZ = 0;

    uint32 Width() const { return MaxX - MinX + 1u; }
    uint32 Height() const { return MaxZ - MinZ + 1u; }
    bool Contains(uint32 x, uint32 z) const { return x >= MinX && x <= MaxX && z >= MinZ && z <= MaxZ; }
};

/// The smallest rect that holds both `a` and `b`.
inline PageSampleRect Union(const PageSampleRect& a, const PageSampleRect& b)
{
    return PageSampleRect{std::min(a.MinX, b.MinX), std::min(a.MinZ, b.MinZ), std::max(a.MaxX, b.MaxX),
                          std::max(a.MaxZ, b.MaxZ)};
}

} // namespace GameEngine::PageStreaming
