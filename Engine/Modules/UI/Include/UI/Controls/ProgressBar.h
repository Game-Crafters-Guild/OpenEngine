#pragma once

#include "UI/Controls/BaseField.h"

namespace GameEngine {

// A track (class progress-bar) with one fill child (progress-bar-fill) whose length is the value's
// share of the range. The fill carries the state class and the track the direction class.
// A bar is an output: SetValue stores the clamped value and sends no change event.
class ProgressBar : public Field<float>
{
public:
    // The edge of the track the fill grows toward.
    enum class FillDirection
    {
        Right,
        Left,
        Up,
        Down,
    };

    ProgressBar();

    // GetValue is the value clamped to [0, max].
    void SetValue(const float& value) override;
    void SetValueWithoutNotify(const float& value) override;

    // The end of the range; 1 by default. A max of 0 or less, or one that is not finite, is
    // refused, and the range stays as it was. A new max clamps the value last set again, so the
    // order of the two attributes in a layout does not matter.
    void SetMax(float max);
    float GetMax() const { return m_Max; }

    // The share of the track an indeterminate bar's segment covers.
    static constexpr float kIndeterminateShare = 0.3f;

    // An indeterminate bar shows a load with no known end: the fill is a segment of
    // kIndeterminateShare centered on the track, and the value is kept but not shown.
    void SetIndeterminate(bool indeterminate);
    bool IsIndeterminate() const { return m_Indeterminate; }

    void SetFillDirection(FillDirection direction);
    FillDirection GetFillDirection() const { return m_Direction; }

private:
    // The share of the range the value fills, from 0 to 1.
    float GetFraction() const { return GetValue() / m_Max; }
    void StoreValue(float value);
    // Returns whether the clamped value changed.
    bool ClampRequestedValue();
    void ApplySegmentPlacement();
    void ApplyFill();

    UIElement* m_Fill = nullptr;
    float m_RequestedValue = 0.0f;
    float m_Max = 1.0f;
    bool m_Indeterminate = false;
    FillDirection m_Direction = FillDirection::Right;
};

} // namespace GameEngine
