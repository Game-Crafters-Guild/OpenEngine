#include "Editor/Hierarchy/HierarchyNavigationBar.h"

#include "Editor/EditorTreeTitleIconVars.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/ItemSizeSlider.h"

#include <cmath>
#include <memory>
#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.uxml";
constexpr const char* kStyleAssetPath = "UI/panels/HierarchyNavigationBar/HierarchyNavigationBar.css";
constexpr float kTreeIconSizeStepPx = 1.0f;
// Sizes closer than this are the same size: the slider's step is a whole pixel.
constexpr float kSameSizeTolerancePx = 0.1f;
} // namespace

HierarchyNavigationBar::HierarchyNavigationBar()
{
    AddClass("hierarchy-navigation-bar");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    EditorUI::ItemSizeSlider::Subject subject{"Row size", "the Hierarchy", "resizes its rows"};
    AddSizeGlyph(subject.Name);

    auto sizeSlider = std::make_unique<EditorUI::ItemSizeSlider>(std::move(subject));
    m_SizeSlider = sizeSlider.get();
    m_SizeSlider->AddClass("hierarchy-size-slider");
    m_SizeSlider->SetMin(kMinEditorTreeIconSizePx);
    m_SizeSlider->SetMax(kMaxEditorHierarchyTreeIconSizePx);
    m_SizeSlider->SetStep(kTreeIconSizeStepPx);
    m_SizeSlider->SetOnValueChanging([this](const float& px) { ReportDrag(px); });
    m_SizeSlider->SetOnValueChanged([this](const float& px)
    {
        if (m_OnItemSizeChanged)
            m_OnItemSizeChanged(px);
    });
    m_SizeSlider->SetOnResizeGesture([this](float scrollY) { ApplyResizeGesture(scrollY); });
    AddChild(std::move(sizeSlider));
}

// The glyph is the bar's layout; the bar is built once per Hierarchy, so the layout is read here.
void HierarchyNavigationBar::AddSizeGlyph(const std::string& tooltip)
{
    std::unique_ptr<UIElement> glyph = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!glyph)
        return;
    glyph->SetTooltip(tooltip);
    AddChild(std::move(glyph));
}

void HierarchyNavigationBar::SetItemSize(float px)
{
    if (m_ReportingDrag || std::fabs(m_SizeSlider->GetValue() - px) <= kSameSizeTolerancePx)
        return;
    m_SizeSlider->SetValueWithoutNotify(px);
}

void HierarchyNavigationBar::ReportDrag(float px)
{
    if (!m_OnItemSizeChanging)
        return;
    m_ReportingDrag = true;
    m_OnItemSizeChanging(px);
    m_ReportingDrag = false;
}

// The same step the gesture takes over the tree, from the size the slider shows.
void HierarchyNavigationBar::ApplyResizeGesture(float scrollY)
{
    const float current = m_SizeSlider->GetValue();
    const float resized = EditorTreeIconSizeAfterResizeGesture(current, scrollY, kMaxEditorHierarchyTreeIconSizePx);
    if (std::fabs(resized - current) > kSameSizeTolerancePx && m_OnItemSizeChanged)
        m_OnItemSizeChanged(resized);
}

void HierarchyNavigationBar::PlaceSearchAbove(UIElement* search)
{
    m_Search = search;
    if (m_Search)
        m_Search->AddClass("hierarchy-panel-search");
}

void HierarchyNavigationBar::SetAboveHorizontalScrollbar(bool above)
{
    if (above)
        AddClass("hierarchy-navigation-bar-above-scrollbar");
    else
        RemoveClass("hierarchy-navigation-bar-above-scrollbar");
    if (!m_Search)
        return;
    if (above)
        m_Search->AddClass("hierarchy-panel-search-above-scrollbar");
    else
        m_Search->RemoveClass("hierarchy-panel-search-above-scrollbar");
}

} // namespace GameEngine::Editor
