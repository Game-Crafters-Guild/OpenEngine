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

// Places a Dropdown or MaskField popup against its header: below it, or above it ("open-upward") when the room
// below is short and the room above is larger; across, it hangs from the header inside the window. `estimatedHeight`
// (layout pixels) stands for the popup's height before its first layout. Each change is applied only when it differs.
DropdownPopupRoom PlaceDropdownPopup(UIElement& header, UIElement& popup, float estimatedHeight);

} // namespace GameEngine::UI::Layout
