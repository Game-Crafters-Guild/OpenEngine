#pragma once

#include "Events/Event.h"
#include "UI/Controls/Slider.h"

#include <functional>
#include <string>

namespace GameEngine::EditorUI
{

// The toolbar slider that sets how big a view draws its items: the Hierarchy's rows, the Assets
// panel's rows and icons. The item resize gesture (UI/Interaction/ItemResizeGesture.h) over the
// slider does what it does over the view the slider sizes, and a plain wheel passes through
// unhandled. The UI manager still delivers the wheel to a disabled element, so a disabled
// slider, or one inside a disabled container, ignores the gesture itself. The tooltip names what
// the slider sizes (Subject) and the gesture's current binding (Keyboard Shortcuts > View
// Navigation > Resize Items), and follows a rebind. The look comes from UI/controls/ItemSizeSlider/ItemSizeSlider.css, requested for this
// slider's own subtree, which also gives every item size slider the same width.
class ItemSizeSlider : public Slider
{
public:
    // What the slider sizes, as its tooltip names it: "<Name>. <binding> over <Area> or this
    // slider also <Effect>."
    struct Subject
    {
        std::string Name = "Item size";
        std::string Area = "the items";
        std::string Effect = "resizes them";
    };

    // Two constructors instead of one defaulted argument: Clang refuses a defaulted argument
    // of a nested aggregate type at the class's own declaration, so the default subject is
    // applied out of line.
    ItemSizeSlider();
    explicit ItemSizeSlider(Subject subject);

    // Receives the gesture's scroll (negative on wheel up). The owner applies the same step
    // the gesture takes over its view, so the slider and the view never disagree.
    void SetOnResizeGesture(std::function<void(float scrollY)> callback)
    {
        m_OnResizeGesture = std::move(callback);
    }

private:
    Subject m_Subject;
    std::function<void(float)> m_OnResizeGesture;
    EventSubscription m_ShortcutBindingsSaved;
};

} // namespace GameEngine::EditorUI
