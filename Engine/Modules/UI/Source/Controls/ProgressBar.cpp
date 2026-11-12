#include "UI/Controls/ProgressBar.h"

#include "UI/StyleProperties.h"
#include "UI/StyleUtil.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace GameEngine {

namespace
{
constexpr const char* kFillIndeterminateClass = "progress-bar-fill-indeterminate";
constexpr const char* kLeftClass = "progress-bar-left";
constexpr const char* kUpClass = "progress-bar-up";
constexpr const char* kDownClass = "progress-bar-down";
constexpr float kPercent = 100.0f;

bool IsVertical(ProgressBar::FillDirection direction)
{
    return direction == ProgressBar::FillDirection::Up || direction == ProgressBar::FillDirection::Down;
}
} // namespace

ProgressBar::ProgressBar()
{
    AddClass("progress-bar");
    auto fill = std::make_unique<UIElement>();
    m_Fill = fill.get();
    m_Fill->AddClass("progress-bar-fill");
    AddChild(std::move(fill));
    ApplyFill();
}

void ProgressBar::SetValue(const float& value)
{
    StoreValue(value);
}

void ProgressBar::SetValueWithoutNotify(const float& value)
{
    StoreValue(value);
}

void ProgressBar::SetMax(float max)
{
    if (!std::isfinite(max) || max <= 0.0f || max == m_Max)
        return;
    m_Max = max;
    ClampRequestedValue();
    ApplyFill();
}

void ProgressBar::SetIndeterminate(bool indeterminate)
{
    if (m_Indeterminate == indeterminate)
        return;
    m_Indeterminate = indeterminate;
    UIUtil::SetClassPresent(*m_Fill, kFillIndeterminateClass, indeterminate);
    ApplySegmentPlacement();
    ApplyFill();
}

void ProgressBar::SetFillDirection(FillDirection direction)
{
    if (m_Direction == direction)
        return;
    m_Direction = direction;
    UIUtil::SetClassPresent(*this, kLeftClass, direction == FillDirection::Left);
    UIUtil::SetClassPresent(*this, kUpClass, direction == FillDirection::Up);
    UIUtil::SetClassPresent(*this, kDownClass, direction == FillDirection::Down);
    ApplySegmentPlacement();
    ApplyFill();
}

void ProgressBar::StoreValue(float value)
{
    m_RequestedValue = value;
    if (ClampRequestedValue())
        ApplyFill();
}

bool ProgressBar::ClampRequestedValue()
{
    // A NaN compares false both ways and lands on 0.
    const float clamped = m_RequestedValue > 0.0f ? std::min(m_RequestedValue, m_Max) : 0.0f;
    if (clamped == GetValue())
        return false;
    Field<float>::SetValueWithoutNotify(clamped);
    return true;
}

// Auto margins on both main-axis edges center the indeterminate segment in every direction.
// No value draws a fill away from the start edge, so the segment never reads as a value.
void ProgressBar::ApplySegmentPlacement()
{
    StyleDeclaration& style = m_Fill->Style();
    style.Reset(Style::MarginLeft).Reset(Style::MarginRight).Reset(Style::MarginTop).Reset(Style::MarginBottom);
    if (!m_Indeterminate)
        return;
    if (IsVertical(m_Direction))
        style.Set(Style::MarginTop, StyleLength::Auto()).Set(Style::MarginBottom, StyleLength::Auto());
    else
        style.Set(Style::MarginLeft, StyleLength::Auto()).Set(Style::MarginRight, StyleLength::Auto());
}

// The fill's main-axis length is the one property a value change sets; its look and its
// cross-axis size come from the style sheets. The state class and the margins change only
// with the state or the direction, so a value change allocates nothing.
void ProgressBar::ApplyFill()
{
    const float share = m_Indeterminate ? kIndeterminateShare : GetFraction();
    const StyleLength length = StyleLength::Percent(share * kPercent);
    if (IsVertical(m_Direction))
        m_Fill->Style().Reset(Style::Width).Set(Style::Height, length);
    else
        m_Fill->Style().Reset(Style::Height).Set(Style::Width, length);
}

} // namespace GameEngine
