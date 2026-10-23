#pragma once

#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <cstddef>

namespace GameEngine::SmartShape2D
{
enum class QuadCorner { None, Inner, Outer };

/// Clockwise source convention: A/B at the start, D/C at the end.
struct EdgeQuad
{
    Mathematics::Vector2 A, B, C, D;
    size_t Part = 0;
    uint32 Group = 0;
    float TileLength = 1.0f;
    float Depth = 0.0f;
    bool Flip = false;
    bool Stretch = true;
    bool Taper = false;
    bool IgnoreWeldNext = false;
    QuadCorner Corner = QuadCorner::None;

    float Length() const;
    float Height() const;
};

EdgeQuad MakeEdgeCorner(Mathematics::Vector2 previous, Mathematics::Vector2 point, Mathematics::Vector2 next,
    float width, float previousWidth, float offset, bool flip, QuadCorner corner);
void WeldEdgeQuads(EdgeQuad& previous, EdgeQuad& next);
/// Splits a taper inside the segment. A short segment is retextured in place.
/// Returns true only when a separate taper quad was produced.
bool SplitEdgeTaper(EdgeQuad& edge, EdgeQuad& taper, Mathematics::Vector2 size, size_t part, bool end);
}
