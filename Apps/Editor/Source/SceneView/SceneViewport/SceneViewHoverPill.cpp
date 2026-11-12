#include "SceneView/SceneViewport/SceneViewHoverPill.h"

#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine
{
namespace
{
// The offset of the pill's top-left corner from the pointer, so the pill does not cover the cursor.
constexpr float kPointerOffsetXPx = 14.0f;
constexpr float kPointerOffsetYPx = 18.0f;
// The gap below the pill at the bottom center, the same as the pane label's bottom inset.
constexpr float kBottomInsetPx = 14.0f;
// The smallest gap between the pill and a label or an edge of the pane.
constexpr float kClearancePx = 4.0f;
// A pill moves above each label it meets; two labels need at most two moves.
constexpr int kLabelLiftPasses = 2;
// A pointer move, or a pane, pill or label box change, smaller than this keeps the placement.
constexpr float kReanchorThresholdPx = 1.0f;

bool MovedByThreshold(float from, float to)
{
    return std::fabs(to - from) >= kReanchorThresholdPx;
}

bool MovedByThreshold(const Mathematics::Rect& from, const Mathematics::Rect& to)
{
    return MovedByThreshold(from.X, to.X) || MovedByThreshold(from.Y, to.Y) ||
           MovedByThreshold(from.Width, to.Width) || MovedByThreshold(from.Height, to.Height);
}

/// True when `pill` comes closer than the gap to `label`. An empty label is never met.
bool Meets(const Mathematics::Rect& pill, const Mathematics::Rect& label)
{
    return !label.IsEmpty() && pill.Overlaps(label.Inflated(kClearancePx));
}

bool MeetsLabel(const Mathematics::Rect& pill, const SceneViewHoverPillLabels& labels)
{
    return Meets(pill, labels.PaneLabel) || Meets(pill, labels.HostLabel);
}

/// The top of `pill` after it moves above each label it meets, in pane pixels.
float LiftAboveLabels(Mathematics::Rect pill, const SceneViewHoverPillLabels& labels)
{
    for (int pass = 0; pass < kLabelLiftPasses; ++pass)
    {
        const Mathematics::Rect* met = Meets(pill, labels.PaneLabel)   ? &labels.PaneLabel
                                       : Meets(pill, labels.HostLabel) ? &labels.HostLabel
                                                                       : nullptr;
        if (!met)
            break;
        // Floor keeps the whole gap above a label at a fractional top.
        pill.Y = std::floor(met->Y - kClearancePx) - pill.Height;
    }
    return pill.Y;
}

/// `value` clamped to [kClearancePx, `limit` - kClearancePx]; the low end wins when the range is empty.
float ClampInsidePane(float value, float limit)
{
    return std::max(kClearancePx, std::min(value, limit - kClearancePx));
}
} // namespace

Mathematics::Vector2 SceneViewHoverPill::Place(bool besidePointer, const Mathematics::Vector2& pointer,
                                               const Mathematics::Vector2& paneSize,
                                               const Mathematics::Vector2& pillSize,
                                               const SceneViewHoverPillLabels& labels)
{
    const float paneWidth = std::round(paneSize.x);
    const float paneHeight = std::round(paneSize.y);
    Mathematics::Rect pill{0.0f, 0.0f, std::round(pillSize.x), std::round(pillSize.y)};
    if (besidePointer)
    {
        pill.X = ClampInsidePane(std::round(pointer.x + kPointerOffsetXPx), paneWidth - pill.Width);
        pill.Y = std::round(pointer.y + kPointerOffsetYPx);
        if (pill.Bottom() > paneHeight - kClearancePx || MeetsLabel(pill, labels))
            pill.Y = std::round(pointer.y - kPointerOffsetYPx) - pill.Height;
    }
    else
    {
        pill.X = ClampInsidePane(std::round((paneWidth - pill.Width) * 0.5f), paneWidth - pill.Width);
        pill.Y = paneHeight - kBottomInsetPx - pill.Height;
    }
    return Mathematics::Vector2(pill.X, ClampInsidePane(LiftAboveLabels(pill, labels), paneHeight - pill.Height));
}

void SceneViewHoverPill::Show(const std::string& text, bool besidePointer, const Mathematics::Vector2& pointer,
                              const SceneViewHoverPillLabels& labels)
{
    if (text != GetText())
    {
        SetText(text);
        RequestRelayout();
        m_PlacedPillWidth = -1.0f;
    }
    Style().Set(Style::Opacity, 1.0f);

    const UIElement* pane = GetParent();
    if (!pane)
        return;
    const Mathematics::Vector2 paneSize(pane->GetLayoutWidth(), pane->GetLayoutHeight());
    const Mathematics::Vector2 pillSize(GetLayoutWidth(), GetLayoutHeight());
    // The pill has no size until its first layout: it is placed in a later frame.
    if (paneSize.x <= 0.0f || paneSize.y <= 0.0f || pillSize.x <= 0.0f || pillSize.y <= 0.0f)
        return;

    const bool placed = m_PlacedPaneSize.x >= 0.0f;
    const bool reanchor = !placed || besidePointer != m_PlacedBesidePointer ||
                          MovedByThreshold(m_PlacedPaneSize.x, paneSize.x) ||
                          MovedByThreshold(m_PlacedPaneSize.y, paneSize.y) ||
                          MovedByThreshold(m_PlacedLabels.PaneLabel, labels.PaneLabel) ||
                          MovedByThreshold(m_PlacedLabels.HostLabel, labels.HostLabel) || m_PlacedPillWidth < 0.0f ||
                          MovedByThreshold(m_PlacedPillWidth, pillSize.x) ||
                          (besidePointer && (MovedByThreshold(m_PlacedPointer.x, pointer.x) ||
                                             MovedByThreshold(m_PlacedPointer.y, pointer.y)));
    if (!reanchor)
        return;

    m_PlacedPaneSize = paneSize;
    m_PlacedPillWidth = pillSize.x;
    m_PlacedLabels = labels;
    m_PlacedBesidePointer = besidePointer;
    m_PlacedPointer = pointer;
    const Mathematics::Vector2 topLeft = Place(besidePointer, pointer, paneSize, pillSize, labels);
    Style()
        .Set(Style::PositionLeft, StyleLength::Px(topLeft.x))
        .Set(Style::PositionTop, StyleLength::Px(topLeft.y))
        .Reset(Style::PositionRight)
        .Reset(Style::PositionBottom);
}

void SceneViewHoverPill::Hide()
{
    m_PlacedPaneSize.x = -1.0f;
    Style().Set(Style::Opacity, 0.0f);
}

} // namespace GameEngine

namespace
{
static auto s_RegisterSceneViewHoverPill =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewHoverPill>(
        "SceneViewHoverPill", []() { return std::make_unique<GameEngine::SceneViewHoverPill>(); })
        .TagAlias("sceneviewhoverpill");
} // namespace
