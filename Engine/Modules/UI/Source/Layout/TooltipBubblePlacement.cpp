#include "TooltipBubblePlacement.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::UI::Layout
{
namespace
{
using Placement = UIElement::TooltipPlacement;

// The whole-pixel start of a bubble of `length` along one axis: centered on `aim` inside the whole
// pixels of the anchor's span [spanStart, spanEnd]. A bubble that does not fit those pixels centers
// on the span, so a scaled layout keeps the span and the aim moves only the arrow.
float CenterInsideSpan(float aim, float length, float spanStart, float spanEnd)
{
    const float lowest = std::ceil(spanStart);
    const float highest = std::floor(spanEnd - length);
    if (lowest <= highest)
        return std::clamp(std::round(aim - length * 0.5f), lowest, highest);
    return std::round((spanStart + spanEnd - length) * 0.5f);
}
} // namespace

TooltipBubblePlacement PlaceTooltipBubble(const PopupRect& viewport,
                                          const PopupRect& anchor,
                                          PopupPosition aim,
                                          float bubbleWidth,
                                          float bubbleHeight,
                                          const TooltipReach& reach,
                                          float margin,
                                          Placement placement)
{
    const float viewLeft = viewport.X + margin;
    const float viewTop = viewport.Y + margin;
    const float viewRight = viewport.X + viewport.Width - margin;
    const float viewBottom = viewport.Y + viewport.Height - margin;
    const float anchorRight = anchor.X + anchor.Width;
    const float anchorBottom = anchor.Y + anchor.Height;

    const float belowY = anchorBottom + reach.Below;
    const float aboveY = anchor.Y - reach.Above - bubbleHeight;
    const float rightX = anchorRight + reach.Right;
    const float leftX = anchor.X - reach.Left - bubbleWidth;
    const bool fitsBelow = belowY + bubbleHeight <= viewBottom;
    const bool fitsAbove = aboveY >= viewTop;
    const bool fitsRight = rightX + bubbleWidth <= viewRight;
    const bool fitsLeft = leftX >= viewLeft;

    Placement side = Placement::Below;
    switch (placement)
    {
    case Placement::Auto:
    {
        const bool roomierAbove = anchor.Y - viewport.Y > viewport.Y + viewport.Height - anchorBottom;
        side = !fitsBelow && (fitsAbove || roomierAbove) ? Placement::Above : Placement::Below;
        break;
    }
    case Placement::Below:
        side = !fitsBelow && fitsAbove ? Placement::Above : Placement::Below;
        break;
    case Placement::Above:
        side = !fitsAbove && fitsBelow ? Placement::Below : Placement::Above;
        break;
    case Placement::Right:
        side = !fitsRight && fitsLeft ? Placement::Left : Placement::Right;
        break;
    case Placement::Left:
        side = !fitsLeft && fitsRight ? Placement::Right : Placement::Left;
        break;
    }

    PopupPosition desired;
    TooltipBubblePlacement placed;
    placed.Side = side;
    if (side == Placement::Below || side == Placement::Above)
    {
        desired.X = CenterInsideSpan(aim.X, bubbleWidth, anchor.X, anchorRight);
        desired.Y = side == Placement::Below ? belowY : aboveY;
        placed.ArrowAim = aim.X;
    }
    else
    {
        desired.X = side == Placement::Right ? rightX : leftX;
        desired.Y = CenterInsideSpan(aim.Y, bubbleHeight, anchor.Y, anchorBottom);
        placed.ArrowAim = aim.Y;
    }
    placed.Position = ClampPopupToViewport(viewport, desired.X, desired.Y, bubbleWidth, bubbleHeight, margin);
    return placed;
}

} // namespace GameEngine::UI::Layout
