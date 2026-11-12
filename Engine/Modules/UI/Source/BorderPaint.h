#pragma once

#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{

// A side of an element's box.
enum class BoxSide : uint8_t
{
    Top,
    Right,
    Bottom,
    Left,
};

// The color of one side of a border.
uint32_t BorderSideColor(const BorderColorsTRBL& colors, BoxSide side);

// The width of one side of a border.
float BorderSideWidth(const Box4& widths, BoxSide side);

} // namespace GameEngine

namespace GameEngine::UI
{

// The colors of a border's sides packed RGBA8 (PackFromARGB), in TRBL order.
struct PackedBorderColors
{
    uint32_t Top = 0u;
    uint32_t Right = 0u;
    uint32_t Bottom = 0u;
    uint32_t Left = 0u;
};

// A solid border as the painter draws it, with widths in device px. A side with
// a width paints.
struct SolidBorderSides
{
    Box4 Widths;
    PackedBorderColors Colors;
};

// Gives `box` the ring of the painted sides that share the most common color
// (on a tie, the first in TRBL order) and returns that color. The ring has
// width 0 on every other side.
uint32_t AddBorderRing(UIPrimitive& box, const SolidBorderSides& border);

// Appends a Rect per painted side in another color than `ring`, and a Triangle
// per square corner between two colors. `boxShape` is the element's Rect.
void AppendBorderSidesInOtherColors(std::vector<UIPrimitive>& prims, const UIPrimitive& boxShape,
                                    const SolidBorderSides& border, uint32_t ring);

// True when two visible colors of a solid border meet at a rounded corner, which
// does not split on its diagonal. The colors are ARGB, as the resolved style holds them.
bool RoundedCornerJoinsTwoBorderColors(const Box4& widths, const BorderColorsTRBL& colors,
                                       const CornerRadiiTLTRBRBL& radii);

} // namespace GameEngine::UI
