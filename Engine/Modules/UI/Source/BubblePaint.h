#pragma once

#include "BorderPaint.h"
#include "Mathematics/Vector2.h"
#include "UI/UIPrimitive.h"

#include <array>
#include <cstdint>

namespace GameEngine::UI
{

// The side of the bubble that an arrow sits on: the arrow points away from it.
BoxSide ArrowEdge(ArrowDirection direction);

// A pointer arrow on the edge of a bubble (a tooltip or a popover) that points at an anchor.
// Physical px. The base of the arrow lies on the outer edge of the bubble's border: Height runs
// from that edge to Tip, and BaseWidth is the length of the base on that edge.
struct PointerArrow
{
    Mathematics::Vector2 Tip;
    ArrowDirection Direction = ArrowDirection::Up;
    float BaseWidth = 0.0f;
    float Height = 0.0f;
    uint32_t FillColor = 0x00000000u;   // packed RGBA8, the bubble's background
    uint32_t BorderColor = 0x00000000u; // packed RGBA8, the bubble's border
    float BorderWidth = 0.0f;           // the bubble's border on this edge; 0 is no border
};

// The triangles of a pointer arrow in draw order, the border then the fill; draw them after the
// bubble. The fill, inset by BorderWidth, covers the bubble's border under the base, so the
// arrow and the bubble show one outline.
struct PointerArrowPrimitives
{
    std::array<UIPrimitive, 2> Triangles{};
    uint32_t Count = 0;
};

PointerArrowPrimitives MakePointerArrow(const PointerArrow& arrow);

} // namespace GameEngine::UI
