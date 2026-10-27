#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace GameEngine::UI::Layout
{

struct PopupRect
{
    float X = 0.0f;
    float Y = 0.0f;
    float Width = 0.0f;
    float Height = 0.0f;
};

struct PopupPosition
{
    float X = 0.0f;
    float Y = 0.0f;
};

inline PopupPosition ClampPopupToViewport(const PopupRect& viewport,
                                          float desiredX,
                                          float desiredY,
                                          float popupWidth,
                                          float popupHeight,
                                          float margin = 0.0f)
{
    const float minX = viewport.X + margin;
    const float minY = viewport.Y + margin;
    const float maxX = std::max(minX, viewport.X + viewport.Width - margin - popupWidth);
    const float maxY = std::max(minY, viewport.Y + viewport.Height - margin - popupHeight);
    return {
        std::clamp(desiredX, minX, maxX),
        std::clamp(desiredY, minY, maxY),
    };
}

// The side of its anchor a popup opens on.
enum class PopupSide : uint8_t
{
    Below,
    Above,
};

struct AnchoredPopupPosition
{
    PopupPosition Position;
    PopupSide Side = PopupSide::Below;
};

// Places a popup `gap` below `anchor` with their left edges level. When the popup does not fit below
// and has more room above, it opens `gap` above the anchor. A given `side` is kept, so the popup does
// not jump when its height changes. The result is clamped to the viewport inset by `margin`.
inline AnchoredPopupPosition PlacePopupBelowOrAbove(const PopupRect& viewport,
                                                    const PopupRect& anchor,
                                                    float popupWidth,
                                                    float popupHeight,
                                                    float margin,
                                                    float gap,
                                                    std::optional<PopupSide> side)
{
    const float anchorBottom = anchor.Y + anchor.Height;
    const float viewportBottom = viewport.Y + viewport.Height;
    if (!side)
    {
        const bool fitsBelow = anchorBottom + gap + popupHeight <= viewportBottom - margin;
        const bool roomierAbove = anchor.Y - viewport.Y > viewportBottom - anchorBottom;
        side = !fitsBelow && roomierAbove ? PopupSide::Above : PopupSide::Below;
    }
    const float desiredY = *side == PopupSide::Above ? anchor.Y - gap - popupHeight : anchorBottom + gap;
    return {ClampPopupToViewport(viewport, anchor.X, desiredY, popupWidth, popupHeight, margin), *side};
}

// Where a submenu opens from: the menu panel that holds its parent row, and that row.
struct SubmenuAnchor
{
    PopupRect ParentPanel; // border box of the panel the parent row sits in
    float RowTop = 0.0f;   // top edge of the parent row
};

// Places a submenu beside its parent panel, never over it. Its left edge sits `gap` past the
// parent panel's right edge; when the right side has no room and the left side has, its right
// edge sits `gap` before the parent panel's left edge, and when neither side has room it takes
// the roomier one. Its first row is level with the parent row: `firstRowInset` is the distance
// from the submenu panel's top edge to its first row (border plus top padding). The result is
// clamped to the viewport inset by `margin`, so a submenu wider than the room on either side
// overlaps its parent rather than leave the window. All values are logical pixels in one space.
inline PopupPosition PlaceSubmenuInViewport(const PopupRect& viewport,
                                            const SubmenuAnchor& anchor,
                                            float popupWidth,
                                            float popupHeight,
                                            float margin,
                                            float gap,
                                            float firstRowInset)
{
    const float viewportLeft = viewport.X + margin;
    const float viewportRight = viewport.X + viewport.Width - margin;
    const float rightX = anchor.ParentPanel.X + anchor.ParentPanel.Width + gap;
    const float leftX = anchor.ParentPanel.X - gap - popupWidth;
    const float roomRight = viewportRight - rightX;
    const float roomLeft = anchor.ParentPanel.X - gap - viewportLeft;

    float desiredX = rightX;
    if (roomRight < popupWidth && (roomLeft >= popupWidth || roomLeft > roomRight))
        desiredX = leftX;

    return ClampPopupToViewport(
        viewport, desiredX, anchor.RowTop - firstRowInset, popupWidth, popupHeight, margin);
}

} // namespace GameEngine::UI::Layout
