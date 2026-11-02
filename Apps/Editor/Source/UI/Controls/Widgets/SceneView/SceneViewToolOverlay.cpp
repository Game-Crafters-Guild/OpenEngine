#include "UI/Controls/Widgets/SceneView/SceneViewToolOverlay.h"

#include "Types/StringId.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <memory>

namespace GameEngine
{

namespace
{

// The FPS label SceneViewPanel adds to the active viewport, by default at its bottom-left corner.
constexpr StringId kFpsLabelId = "SceneViewFpsLabel"_sid;

bool SpansOverlap(float startA, float lengthA, float startB, float lengthB)
{
    return startA < startB + lengthB && startB < startA + lengthA;
}

} // namespace

void SceneViewToolOverlay::OnPostLayout()
{
    // A dragged or restored strip has a left set in code, and the user's place wins.
    UIElement* viewport = GetParent();
    if (!viewport || Style().Has(Style::PositionLeft.id))
        return;
    const float height = GetLayoutHeight();
    const float viewportHeight = viewport->GetLayoutHeight();
    if (height <= 0.0f || viewportHeight <= 0.0f)
        return;

    // The stylesheet's left inset is also the gap below the viewport's top and above the FPS label.
    const float inset = GetLayoutX() - viewport->GetLayoutX();
    const float centeredTop = (viewportHeight - height) * 0.5f;
    const float lowestTop = LowestBottom(*viewport, inset) - height;
    if (centeredTop <= lowestTop)
    {
        if (m_HasPinnedTop)
        {
            Style().Reset(Style::PositionTop);
            m_HasPinnedTop = false;
        }
        return;
    }

    const float top = std::max(inset, lowestTop);
    if (m_HasPinnedTop && top == m_PinnedTop)
        return;
    Style().Set(Style::PositionTop, StyleLength::Px(top));
    m_HasPinnedTop = true;
    m_PinnedTop = top;
}

float SceneViewToolOverlay::LowestBottom(UIElement& viewport, float inset) const
{
    const float viewportHeight = viewport.GetLayoutHeight();
    const float lowest = viewportHeight - inset;
    const UIElement* label = viewport.FindById(kFpsLabelId);
    if (!label || label->GetResolvedStyle().Visual.Opacity <= 0.0f)
        return lowest;

    // A label the user dragged to the upper half or out of the strip's column is not under it.
    const float labelTop = label->GetLayoutY() - viewport.GetLayoutY();
    const bool inLowerHalf = labelTop >= viewportHeight * 0.5f;
    if (!inLowerHalf || !SpansOverlap(GetLayoutX(), GetLayoutWidth(), label->GetLayoutX(), label->GetLayoutWidth()))
        return lowest;
    return std::min(lowest, labelTop - inset);
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_sceneViewToolOverlay =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewToolOverlay>(
        "SceneViewToolOverlay",
        []() { return std::make_unique<GameEngine::SceneViewToolOverlay>(); })
    .TagAlias("sceneviewtooloverlay");
} // namespace
