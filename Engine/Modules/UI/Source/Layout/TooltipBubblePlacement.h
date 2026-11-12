#pragma once

#include "UI/Layout/PopupPlacement.h"
#include "UI/UIElement.h"

namespace GameEngine::UI::Layout
{

// The distance from each edge of a tooltip's anchor to the bubble on that side: the gap and the arrow.
struct TooltipReach
{
    float Below = 0.0f;
    float Above = 0.0f;
    float Right = 0.0f;
    float Left = 0.0f;
};

struct TooltipBubblePlacement
{
    PopupPosition Position; // the bubble's top-left corner
    // The side the bubble sits on, never Auto.
    UIElement::TooltipPlacement Side = UIElement::TooltipPlacement::Below;
    // Where the arrow points along the bubble's edge that faces the anchor: x on Below and Above,
    // y on Right and Left.
    float ArrowAim = 0.0f;
};

// Places the bubble `reach` past one side of `anchor`; a side with no room flips when the opposite side has room.
// Auto: below, else above, else the roomier one. Along the edge it centers on `aim` on a whole pixel, inside the
// anchor's span when it fits, then inside `viewport` less `margin`. Logical pixels.
TooltipBubblePlacement PlaceTooltipBubble(const PopupRect& viewport,
                                          const PopupRect& anchor,
                                          PopupPosition aim,
                                          float bubbleWidth,
                                          float bubbleHeight,
                                          const TooltipReach& reach,
                                          float margin,
                                          UIElement::TooltipPlacement placement);

} // namespace GameEngine::UI::Layout
