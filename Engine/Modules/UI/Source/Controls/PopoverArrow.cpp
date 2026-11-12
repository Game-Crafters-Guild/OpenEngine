#include "UI/Controls/PopoverArrow.h"

#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"
#include "../BubblePaint.h"

namespace GameEngine
{

PopoverArrow::PopoverArrow()
{
    AddClass("popover-arrow");
    Style().Set(Style::PointerEvents, false);
}

void PopoverArrow::SetSide(UI::ArrowDirection side)
{
    if (m_Side == side)
        return;
    m_Side = side;
    MarkDirty(VisualDirty);
}

void PopoverArrow::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
                                        float x, float y, float W, float H)
{
    const UIElement* bubble = GetParent();
    if (!bubble || W <= 0.0f || H <= 0.0f)
        return;

    // The bubble's rect in this element's physical px: the offset between the two layout
    // rects does not depend on scrolling or on where the tree is drawn.
    const float cs = ctx.ContentScale;
    const float bubbleX = x + (bubble->GetLayoutX() - GetLayoutX()) * cs;
    const float bubbleY = y + (bubble->GetLayoutY() - GetLayoutY()) * cs;
    const float bubbleW = bubble->GetLayoutWidth() * cs;
    const float bubbleH = bubble->GetLayoutHeight() * cs;

    const ResolvedStyle& bubbleStyle = bubble->GetResolvedStyle();
    const Box4& borders = bubbleStyle.Layout.BorderWidth;
    UI::PointerArrow arrow;
    arrow.Direction = m_Side;
    float border = 0.0f;
    switch (m_Side)
    {
    case UI::ArrowDirection::Down:
        arrow.Tip = {x + W * 0.5f, y + H};
        arrow.Height = arrow.Tip.y - (bubbleY + bubbleH);
        arrow.BaseWidth = W;
        border = borders.Bottom;
        break;
    case UI::ArrowDirection::Left:
        arrow.Tip = {x, y + H * 0.5f};
        arrow.Height = bubbleX - arrow.Tip.x;
        arrow.BaseWidth = H;
        border = borders.Left;
        break;
    case UI::ArrowDirection::Right:
        arrow.Tip = {x + W, y + H * 0.5f};
        arrow.Height = arrow.Tip.x - (bubbleX + bubbleW);
        arrow.BaseWidth = H;
        border = borders.Right;
        break;
    case UI::ArrowDirection::Up:
    default:
        arrow.Tip = {x + W * 0.5f, y};
        arrow.Height = bubbleY - arrow.Tip.y;
        arrow.BaseWidth = W;
        border = borders.Top;
        break;
    }
    arrow.FillColor = UI::PackFromARGB(bubbleStyle.Visual.BackgroundColor);
    if (bubbleStyle.Visual.BorderStyle != BorderStyle::None)
    {
        arrow.BorderColor = UI::PackFromARGB(BorderSideColor(bubbleStyle.Visual.BorderColor, UI::ArrowEdge(arrow.Direction)));
        arrow.BorderWidth = border * cs;
    }

    const UI::PointerArrowPrimitives triangles = UI::MakePointerArrow(arrow);
    for (uint32_t i = 0; i < triangles.Count; ++i)
        ctx.Emit(triangles.Triangles[i]);
}

} // namespace GameEngine
