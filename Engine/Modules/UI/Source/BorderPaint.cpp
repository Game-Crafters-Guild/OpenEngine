#include "BorderPaint.h"

#include <array>

namespace GameEngine
{

uint32_t BorderSideColor(const BorderColorsTRBL& colors, BoxSide side)
{
    switch (side)
    {
    case BoxSide::Top:
        return colors.Top;
    case BoxSide::Right:
        return colors.Right;
    case BoxSide::Bottom:
        return colors.Bottom;
    case BoxSide::Left:
    default:
        return colors.Left;
    }
}

float BorderSideWidth(const Box4& widths, BoxSide side)
{
    switch (side)
    {
    case BoxSide::Top:
        return widths.Top;
    case BoxSide::Right:
        return widths.Right;
    case BoxSide::Bottom:
        return widths.Bottom;
    case BoxSide::Left:
    default:
        return widths.Left;
    }
}

} // namespace GameEngine

namespace GameEngine::UI
{

namespace
{

constexpr uint32_t kAlphaMask = 0xFF000000u;
constexpr std::array<BoxSide, 4> kSides = {BoxSide::Top, BoxSide::Right, BoxSide::Bottom, BoxSide::Left};

// A corner of the box: the two sides that meet there, and the index of its
// radius in UIPrimitive::Radii (TL, TR, BR, BL).
struct BoxCorner
{
    BoxSide Horizontal;
    BoxSide Vertical;
    size_t Radius;
};

constexpr std::array<BoxCorner, 4> kCorners = {{
    {BoxSide::Top, BoxSide::Left, 0},
    {BoxSide::Top, BoxSide::Right, 1},
    {BoxSide::Bottom, BoxSide::Right, 2},
    {BoxSide::Bottom, BoxSide::Left, 3},
}};

float Width(const SolidBorderSides& border, BoxSide side)
{
    return BorderSideWidth(border.Widths, side);
}

uint32_t Color(const SolidBorderSides& border, BoxSide side)
{
    switch (side)
    {
    case BoxSide::Top:
        return border.Colors.Top;
    case BoxSide::Right:
        return border.Colors.Right;
    case BoxSide::Bottom:
        return border.Colors.Bottom;
    case BoxSide::Left:
    default:
        return border.Colors.Left;
    }
}

// ARGB and packed RGBA8 both hold the alpha in the top byte.
bool IsTransparent(uint32_t color)
{
    return (color & kAlphaMask) == 0u;
}

// Two sides paint two colors where they meet: both have a width, and the colors
// differ and are not transparent. A transparent side leaves the corner to the other.
bool JoinsTwoVisibleColors(float widthA, uint32_t colorA, float widthB, uint32_t colorB)
{
    return widthA > 0.0f && widthB > 0.0f && colorA != colorB && !IsTransparent(colorA) && !IsTransparent(colorB);
}

uint32_t RingColor(const SolidBorderSides& border)
{
    uint32_t ring = 0u;
    int ringSides = 0;
    for (BoxSide side : kSides)
    {
        if (Width(border, side) <= 0.0f)
            continue;
        int sides = 0;
        for (BoxSide other : kSides)
            sides += Width(border, other) > 0.0f && Color(border, other) == Color(border, side) ? 1 : 0;
        if (sides > ringSides)
        {
            ring = Color(border, side);
            ringSides = sides;
        }
    }
    return ring;
}

// The width of `side` in a band that paints only `painted`.
float BandWidth(const SolidBorderSides& border, BoxSide side, BoxSide painted)
{
    return side == painted ? Width(border, side) : 0.0f;
}

float RingWidth(const SolidBorderSides& border, uint32_t ring, BoxSide side)
{
    return Color(border, side) == ring ? Width(border, side) : 0.0f;
}

// The ring draws first, then the sides in other colors in TRBL order.
int DrawOrder(const SolidBorderSides& border, uint32_t ring, BoxSide side)
{
    return Color(border, side) == ring ? -1 : static_cast<int>(side);
}

// The half of the corner square next to the side `earlier`: the outer corner,
// the inner corner, and the point on the outer edge of `earlier` between them.
void AppendCornerTriangle(std::vector<UIPrimitive>& prims, const UIPrimitive& boxShape,
                          const SolidBorderSides& border, const BoxCorner& corner, BoxSide earlier)
{
    const bool left = corner.Vertical == BoxSide::Left;
    const bool top = corner.Horizontal == BoxSide::Top;
    const float outerX = left ? boxShape.X : boxShape.X + boxShape.W;
    const float outerY = top ? boxShape.Y : boxShape.Y + boxShape.H;
    const float innerX = left ? outerX + border.Widths.Left : outerX - border.Widths.Right;
    const float innerY = top ? outerY + border.Widths.Top : outerY - border.Widths.Bottom;
    const bool horizontalFirst = earlier == corner.Horizontal;
    UIPrimitive triangle = MakeTriangle(outerX, outerY, horizontalFirst ? innerX : outerX,
                                        horizontalFirst ? outerY : innerY, innerX, innerY,
                                        Color(border, earlier), 0.0f, GetClipIndex(boxShape.ModeAndFlags));
    SetOpacity(triangle, boxShape.Opacity);
    prims.push_back(triangle);
}

} // namespace

uint32_t AddBorderRing(UIPrimitive& box, const SolidBorderSides& border)
{
    const uint32_t ring = RingColor(border);
    AddBorderLTRB(box, RingWidth(border, ring, BoxSide::Left), RingWidth(border, ring, BoxSide::Top),
                  RingWidth(border, ring, BoxSide::Right), RingWidth(border, ring, BoxSide::Bottom), ring);
    return ring;
}

void AppendBorderSidesInOtherColors(std::vector<UIPrimitive>& prims, const UIPrimitive& boxShape,
                                    const SolidBorderSides& border, uint32_t ring)
{
    for (BoxSide side : kSides)
    {
        if (Width(border, side) <= 0.0f || Color(border, side) == ring || IsTransparent(Color(border, side)))
            continue;
        UIPrimitive band = boxShape;
        band.FillColor = 0u;
        AddBorderLTRB(band, BandWidth(border, BoxSide::Left, side), BandWidth(border, BoxSide::Top, side),
                      BandWidth(border, BoxSide::Right, side), BandWidth(border, BoxSide::Bottom, side),
                      Color(border, side));
        prims.push_back(band);
    }

    for (const BoxCorner& corner : kCorners)
    {
        if (!JoinsTwoVisibleColors(Width(border, corner.Horizontal), Color(border, corner.Horizontal),
                                   Width(border, corner.Vertical), Color(border, corner.Vertical)))
            continue;
        // A rounded corner does not split: the later side paints over the earlier one.
        if (boxShape.Radii[corner.Radius] > 0.0f || boxShape.RadiiY[corner.Radius] > 0.0f)
            continue;
        const bool horizontalLater = DrawOrder(border, ring, corner.Horizontal) > DrawOrder(border, ring, corner.Vertical);
        AppendCornerTriangle(prims, boxShape, border, corner, horizontalLater ? corner.Vertical : corner.Horizontal);
    }
}

bool RoundedCornerJoinsTwoBorderColors(const Box4& widths, const BorderColorsTRBL& colors,
                                       const CornerRadiiTLTRBRBL& radii)
{
    const std::array<CornerRadius, 4> cornerRadii = {radii.TopLeft, radii.TopRight, radii.BottomRight,
                                                     radii.BottomLeft};
    for (const BoxCorner& corner : kCorners)
    {
        const CornerRadius& radius = cornerRadii[corner.Radius];
        if (radius.X <= 0.0f && radius.Y <= 0.0f)
            continue;
        if (JoinsTwoVisibleColors(BorderSideWidth(widths, corner.Horizontal), BorderSideColor(colors, corner.Horizontal),
                                  BorderSideWidth(widths, corner.Vertical), BorderSideColor(colors, corner.Vertical)))
            return true;
    }
    return false;
}

} // namespace GameEngine::UI
