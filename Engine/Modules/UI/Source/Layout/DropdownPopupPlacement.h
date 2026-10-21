#pragma once

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::UI::Layout
{

// The room a dropdown-style popup was given, on the side it opens.
struct DropdownPopupRoom
{
    // Window pixels between the header and the edge of the visible area on that side.
    float Height = 0.0f;
    // Window pixels per layout pixel of the header on the vertical axis: a CSS transform scales the popup
    // with its header.
    float WindowPerLayoutY = 1.0f;
};

// Places a dropdown-style popup (Dropdown, MaskField) against its header. It opens below the header, or
// above it when the room below is short and the room above is larger ("open-upward" on the header). A
// popup wider than the room to the header's right is pulled back inside the visible area and given its
// width. The visible area is the window cut down to the scroll viewports and dock panels around the
// header, measured where they are drawn. `estimatedHeight` (layout pixels) stands for the popup's height
// before it has been laid out. Each change is applied only when it differs from what stands, because a
// class flipped off and on again costs the tree another post-layout pass and solve.
DropdownPopupRoom PlaceDropdownPopup(UIElement& header, UIElement& popup, float estimatedHeight);

} // namespace GameEngine::UI::Layout
