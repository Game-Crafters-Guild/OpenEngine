#include "UI/Interaction/FocusQueries.h"

#include "UI/Controls/TextArea.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <string>

namespace GameEngine::UI
{

namespace
{

// Depth budget on an ancestor walk, as the event bubble and the hover descendant test carry:
// a malformed parent cycle must not hang a keystroke.
constexpr int kMaxAncestorDepth = 2048;

// A text field takes the focus for its text input, which passes its focus to the field.
bool EditsText(UIElement& element)
{
    if (element.GetAsTextInput() || dynamic_cast<const TextArea*>(&element))
        return true;
    for (const auto& child : element.GetChildren())
    {
        if (child->GetAsTextInput() && child->GetFocusProxy() == &element)
            return true;
    }
    return false;
}

} // namespace

bool FocusIsInside(const UIElement& element)
{
    const UIManager* manager = element.GetOwnerManager();
    if (!manager)
        return false;

    const std::string& focusId = manager->GetFocusedElementId();
    if (focusId.empty())
        return false;

    // Resolve the id from the root exactly as the dispatcher does, then walk
    // up: an id that also exists inside this element is not focus unless it is
    // the element the manager actually resolved.
    UIElement* root = manager->GetRootElement();
    if (!root)
        return false;

    int depth = 0;
    for (const UIElement* el = root->FindById(HashStringId(focusId)); el && depth++ < kMaxAncestorDepth;
         el = el->GetParent())
    {
        if (el == &element)
            return true;
    }
    return false;
}

bool FocusIsInTextInput(const UIManager& manager, const UIElement* within)
{
    const std::string& focusId = manager.GetFocusedElementId();
    if (focusId.empty())
        return false;
    UIElement* root = manager.GetRootElement();
    if (!root)
        return false;

    bool editsText = false;
    bool inside = within == nullptr;
    int depth = 0;
    for (UIElement* el = root->FindById(HashStringId(focusId)); el && depth++ < kMaxAncestorDepth;
         el = el->GetParent())
    {
        editsText = editsText || EditsText(*el);
        inside = inside || el == within;
    }
    return editsText && inside;
}

} // namespace GameEngine::UI
