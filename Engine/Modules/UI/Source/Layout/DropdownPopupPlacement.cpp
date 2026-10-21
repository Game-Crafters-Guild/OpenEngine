#include "DropdownPopupPlacement.h"

#include "Mathematics/Rect.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>

namespace GameEngine::UI::Layout
{
namespace
{
constexpr float kPopupEdgeClearance = 2.0f;

// The window-space area the popup may draw in: the root's window rect cut down to every ancestor that is a
// ScrollView clip viewport or a dock panel box. Those impose a real limit whatever their content; a plain
// overflow:hidden wrapper sizes to its content and clips nothing, so it does not count.
Mathematics::Rect VisibleArea(const UIElement& header, const UIElement& root)
{
    const Mathematics::Rect window = root.GetWindowRect();
    float top = window.Y;
    float bottom = window.Bottom();
    float left = window.X;
    float right = window.Right();
    for (UIElement* ancestor = header.GetParent(); ancestor; ancestor = ancestor->GetParent())
    {
        if (!ancestor->HasClass("scroll-viewport") && !ancestor->HasClass("panel"))
            continue;
        const Mathematics::Rect bounds = ancestor->GetWindowRect();
        top = std::max(top, bounds.Y);
        bottom = std::min(bottom, bounds.Bottom());
        left = std::max(left, bounds.X);
        right = std::min(right, bounds.Right());
    }
    return Mathematics::Rect::FromEdges(left, top, right, bottom);
}

void SetUpward(UIElement& header, bool upward)
{
    if (upward)
        header.AddClass("open-upward");
    else
        header.RemoveClass("open-upward");
}
} // namespace

DropdownPopupRoom PlaceDropdownPopup(UIElement& header, UIElement& popup, float estimatedHeight)
{
    UIManager* manager = header.GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!root)
    {
        SetUpward(header, false);
        return {};
    }

    const Mathematics::Rect visible = VisibleArea(header, *root);
    const Mathematics::Rect rect = header.GetWindowRect();
    // The popup lays out against the header and paints in the header's transform, so its lengths reach the
    // window multiplied by the header's scale on each axis, and a window-space offset goes back divided by it.
    const float windowPerLayoutX = header.GetLayoutWidth() > 0.0f ? rect.Width / header.GetLayoutWidth() : 1.0f;
    const float windowPerLayoutY = header.GetLayoutHeight() > 0.0f ? rect.Height / header.GetLayoutHeight() : 1.0f;
    const float roomAbove = std::max(0.0f, rect.Y - visible.Y);
    const float roomBelow = std::max(0.0f, visible.Bottom() - rect.Bottom());

    // A closed popup has no layout height yet: the estimate stands in for its first opening.
    const float measuredHeight = popup.GetLayoutHeight() * windowPerLayoutY;
    const float requiredHeight =
        measuredHeight > 1.0f ? measuredHeight + kPopupEdgeClearance : estimatedHeight * windowPerLayoutY;
    const bool upward = roomBelow < requiredHeight && roomAbove > roomBelow;
    SetUpward(header, upward);

    const float available = visible.Right() - visible.X - 2.0f * kPopupEdgeClearance;
    // Before its first layout the popup has no width of its own; it is at least as wide as its header.
    const float popupWidth = std::max(popup.GetLayoutWidth() * windowPerLayoutX, rect.Width);
    const float overflowRight = (rect.X + popupWidth) - (visible.Right() - kPopupEdgeClearance);
    float offset = overflowRight > 0.0f ? -overflowRight : 0.0f;
    const float leftLimit = (visible.X + kPopupEdgeClearance) - rect.X;
    offset = std::max(offset, std::min(0.0f, leftLimit));
    if (offset < 0.0f)
    {
        popup.Overrides().Set(Style::PositionLeft, StyleLength::Px(offset / windowPerLayoutX));
        // A popup that has to be pulled back does not fit where it was anchored, and it was sized against
        // the header, not the panel: give it the panel's width so its rows use the space that is there.
        if (available > 0.0f)
            popup.Overrides().Set(Style::MinWidth, StyleLength::Px(available / windowPerLayoutX));
    }
    else
    {
        popup.Overrides().Reset(Style::PositionLeft);
        popup.Overrides().Reset(Style::MinWidth);
    }

    return {std::max(0.0f, (upward ? roomAbove : roomBelow) - kPopupEdgeClearance), windowPerLayoutY};
}

} // namespace GameEngine::UI::Layout
