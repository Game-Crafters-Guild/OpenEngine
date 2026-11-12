#include "BubblePaint.h"

#include <cmath>

namespace GameEngine::UI
{

namespace
{

using Mathematics::Vector2;

// How far the fill triangle reaches past the bubble's border into its content, physical px:
// one anti-aliasing band, so the soft edge of the fill's base lies on the bubble's own fill.
constexpr float kFillOverlap = 1.0f;

// The arrow's frame: Axis runs from the tip toward the bubble, Across along the base.
struct ArrowFrame
{
    Vector2 Tip;
    Vector2 Axis;
    Vector2 Across;
};

ArrowFrame MakeFrame(const PointerArrow& arrow)
{
    switch (arrow.Direction)
    {
    case ArrowDirection::Down:
        return {arrow.Tip, Vector2(0.0f, -1.0f), Vector2(1.0f, 0.0f)};
    case ArrowDirection::Left:
        return {arrow.Tip, Vector2(1.0f, 0.0f), Vector2(0.0f, 1.0f)};
    case ArrowDirection::Right:
        return {arrow.Tip, Vector2(-1.0f, 0.0f), Vector2(0.0f, 1.0f)};
    case ArrowDirection::Up:
    default:
        return {arrow.Tip, Vector2(0.0f, 1.0f), Vector2(1.0f, 0.0f)};
    }
}

// An isosceles triangle in the frame: its apex `apex` along the axis from the tip, its base
// `base` along the axis, `slope` half-widths per unit along the axis.
UIPrimitive MakeFrameTriangle(const ArrowFrame& frame, float apex, float base, float slope, uint32_t color)
{
    const float halfBase = (base - apex) * slope;
    const Vector2 apexPoint = frame.Tip + frame.Axis * apex;
    const Vector2 baseCenter = frame.Tip + frame.Axis * base;
    const Vector2 baseA = baseCenter - frame.Across * halfBase;
    const Vector2 baseB = baseCenter + frame.Across * halfBase;
    return MakeTriangle(apexPoint.x, apexPoint.y, baseA.x, baseA.y, baseB.x, baseB.y, color);
}

} // namespace

BoxSide ArrowEdge(ArrowDirection direction)
{
    switch (direction)
    {
    case ArrowDirection::Down:
        return BoxSide::Bottom;
    case ArrowDirection::Left:
        return BoxSide::Left;
    case ArrowDirection::Right:
        return BoxSide::Right;
    case ArrowDirection::Up:
    default:
        return BoxSide::Top;
    }
}

PointerArrowPrimitives MakePointerArrow(const PointerArrow& arrow)
{
    PointerArrowPrimitives out;
    if (arrow.Height <= 0.0f || arrow.BaseWidth <= 0.0f)
        return out;

    const ArrowFrame frame = MakeFrame(arrow);
    const float slope = arrow.BaseWidth * 0.5f / arrow.Height;
    const bool hasBorder = arrow.BorderWidth > 0.0f && (arrow.BorderColor >> 24) != 0;
    const float border = hasBorder ? arrow.BorderWidth : 0.0f;

    // The border triangle reaches across the bubble's border, so the two strokes join.
    if (hasBorder)
        out.Triangles[out.Count++] = MakeFrameTriangle(frame, 0.0f, arrow.Height + border, slope, arrow.BorderColor);

    // The fill triangle: the slanted sides moved in by the border width, the base past the
    // bubble's border into its content.
    if ((arrow.FillColor >> 24) == 0)
        return out;
    const float apex = border * std::sqrt(1.0f + slope * slope) / slope;
    const float base = arrow.Height + border + kFillOverlap;
    if (apex < base)
        out.Triangles[out.Count++] = MakeFrameTriangle(frame, apex, base, slope, arrow.FillColor);
    return out;
}

} // namespace GameEngine::UI
