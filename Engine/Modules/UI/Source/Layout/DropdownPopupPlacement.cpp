#include "DropdownPopupPlacement.h"

#include "Mathematics/Rect.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace GameEngine::UI::Layout
{
namespace
{
constexpr float kPopupEdgeClearance = 2.0f;
// A laid-out width within this of the cap is the cap.
constexpr float kCapTolerance = 0.5f;

// The window-space area the popup may open into on the vertical axis: the root's window rect cut down to
// every ancestor that is a ScrollView clip viewport or a dock panel box. Those impose a real limit whatever
// their content; a plain overflow:hidden wrapper sizes to its content and clips nothing, so it does not count.
// The horizontal limit is the window (PlaceHorizontally).
Mathematics::Rect VisibleArea(const UIElement& header, const UIElement& root)
{
    const Mathematics::Rect window = root.GetWindowRect();
    float top = window.Y;
    float bottom = window.Bottom();
    for (UIElement* ancestor = header.GetParent(); ancestor; ancestor = ancestor->GetParent())
    {
        if (!ancestor->HasClass("scroll-viewport") && !ancestor->HasClass("panel"))
            continue;
        const Mathematics::Rect bounds = ancestor->GetWindowRect();
        top = std::max(top, bounds.Y);
        bottom = std::min(bottom, bounds.Bottom());
    }
    return Mathematics::Rect::FromEdges(window.X, top, window.Right(), bottom);
}

void SetUpward(UIElement& header, bool upward)
{
    if (upward)
        header.AddClass("open-upward");
    else
        header.RemoveClass("open-upward");
}

// The list hangs from the header's left edge, or from its right edge when that leaves the window; a list wider
// than the room on both sides takes the larger room and its rows end in an ellipsis. The window is the limit, not
// the panel. Lengths are window pixels; `windowPerLayoutX` converts them back for the popup's style.
void PlaceHorizontally(const Mathematics::Rect& header, const Mathematics::Rect& window, UIElement& popup,
                       float windowPerLayoutX)
{
    const float roomFromLeftEdge = std::max(header.Width, window.Right() - kPopupEdgeClearance - header.X);
    const float roomFromRightEdge = std::max(header.Width, header.Right() - (window.X + kPopupEdgeClearance));

    // Before its first layout the popup has no width of its own; it is at least as wide as its header.
    const float laidOutWidth = std::max(popup.GetLayoutWidth() * windowPerLayoutX, header.Width);
    const std::optional<StyleLength> cap = popup.Style().Get(Style::MaxWidth);
    const float capWidth = cap ? cap->Value * windowPerLayoutX : 0.0f;
    // A capped list shows the cap, not its entries' width: it keeps the cap until the room changes.
    const bool capped = cap && laidOutWidth >= capWidth - kCapTolerance;
    const float largerRoom = std::max(roomFromLeftEdge, roomFromRightEdge);

    float width = laidOutWidth;
    bool fromRightEdge = false;
    if (capped && std::abs(largerRoom - capWidth) <= kCapTolerance)
    {
        width = capWidth;
        fromRightEdge = roomFromRightEdge > roomFromLeftEdge;
    }
    else if (capped || laidOutWidth <= roomFromLeftEdge)
    {
        // A capped list whose room changed is measured again at its entries' width on the next pass.
        popup.Style().Reset(Style::MaxWidth);
    }
    else if (laidOutWidth <= roomFromRightEdge)
    {
        popup.Style().Reset(Style::MaxWidth);
        fromRightEdge = true;
    }
    else
    {
        width = largerRoom;
        fromRightEdge = roomFromRightEdge > roomFromLeftEdge;
        popup.Style().Set(Style::MaxWidth, StyleLength::Px(largerRoom / windowPerLayoutX));
    }

    if (fromRightEdge)
        popup.Style().Set(Style::PositionLeft, StyleLength::Px((header.Width - width) / windowPerLayoutX));
    else
        popup.Style().Reset(Style::PositionLeft);
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

    PlaceHorizontally(rect, root->GetWindowRect(), popup, windowPerLayoutX);

    return {std::max(0.0f, (upward ? roomAbove : roomBelow) - kPopupEdgeClearance), windowPerLayoutY};
}

} // namespace GameEngine::UI::Layout
