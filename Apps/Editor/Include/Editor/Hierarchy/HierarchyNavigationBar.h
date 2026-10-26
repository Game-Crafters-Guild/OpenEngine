#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{
namespace EditorUI
{
class ItemSizeSlider;
}

namespace Editor
{

// The Hierarchy's bottom bar: a rows glyph, then the item size slider, pinned to the panel's
// bottom edge wherever the panel search sits (the search is the panel's own child and moves with
// the search bar placement setting). The glyph is
// UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.uxml; the look comes from
// HierarchyNavigationBar.css beside it, requested for this bar's subtree.
//
// The slider sets the Hierarchy's tree icon size. The bar reports sizes through two callbacks and
// is told the current size back; the panel owns the size.
class HierarchyNavigationBar : public UIElement
{
public:
    HierarchyNavigationBar();

    // The size the slider shows. Raises neither callback, and leaves the slider alone while it is
    // reporting a drag, so the drag keeps its thumb.
    void SetItemSize(float px);

    // While the slider drags: the size under the thumb.
    void SetOnItemSizeChanging(std::function<void(float)> callback) { m_OnItemSizeChanging = std::move(callback); }

    // A size to commit: the slider released, or the item resize gesture over the slider.
    void SetOnItemSizeChanged(std::function<void(float)> callback) { m_OnItemSizeChanged = std::move(callback); }

    // The panel's search, a sibling of this bar: while searches sit at the bottom it sits above
    // this bar, and it lifts with the bar over the horizontal scrollbar. The panel owns both, so
    // the search outlives every call on this bar.
    void PlaceSearchAbove(UIElement* search);

    // Lifts the bar, and the search placed above it, over the tree's horizontal scrollbar while
    // one is on screen.
    void SetAboveHorizontalScrollbar(bool above);

private:
    void AddSizeGlyph(const std::string& tooltip);
    void ReportDrag(float px);
    void ApplyResizeGesture(float scrollY);

    EditorUI::ItemSizeSlider* m_SizeSlider = nullptr;
    UIElement* m_Search = nullptr;
    std::function<void(float)> m_OnItemSizeChanging;
    std::function<void(float)> m_OnItemSizeChanged;
    bool m_ReportingDrag = false;
};

} // namespace Editor
} // namespace GameEngine
