#include "Gamepad/ControllerMapping/ControllerMapping.h"

#include <algorithm>

namespace GameEngine::Input
{
namespace
{

using Reading = ControllerMapping::Reading;
using Source = ControllerMapping::Source;

// A source's reading as a fraction of its range, 0 to 1: a button or a hat direction reads 0 or 1;
// a full axis reads 0 at -1 and 1 at +1; a half axis reads 0 at the center and 1 at its end. An
// inverted source reads the reverse.
struct SourceReading
{
    float Fraction = 0.0f;
    bool Digital = false;
};

SourceReading ReadSource(const Source& source, const Reading& reading)
{
    switch (source.SourceKind)
    {
    case Source::Kind::Button:
    {
        const bool down = source.Index < reading.Buttons.size() && reading.Buttons[source.Index];
        return {down ? 1.0f : 0.0f, true};
    }
    case Source::Kind::Hat:
    {
        const bool down = source.Index < reading.Hats.size() && (reading.Hats[source.Index] & source.HatMask) != 0;
        return {down ? 1.0f : 0.0f, true};
    }
    case Source::Kind::Axis:
    {
        const bool present = source.Index < reading.Axes.size();
        const float value = present ? std::clamp(reading.Axes[source.Index], -1.0f, 1.0f) : 0.0f;
        if (source.Range == Source::AxisRange::Full)
            return {((source.Inverted ? -value : value) + 1.0f) * 0.5f, false};
        // "~" reverses a half axis inside its own half; a value outside the half reads 0.
        const float half = source.Range == Source::AxisRange::Positive ? value : -value;
        if (!present || half < 0.0f)
            return {};
        return {source.Inverted ? 1.0f - half : half, false};
    }
    case Source::Kind::None:
        break;
    }
    return {};
}

// A button is down past the middle of its source's range. A full axis at rest (the center) reads up.
bool ReadButton(const Source& source, const Reading& reading)
{
    return source.SourceKind != Source::Kind::None && ReadSource(source, reading).Fraction > 0.5f;
}

// The source's reading mapped onto [restValue, endValue]. A released digital source reads 0, so a
// button bound to a whole stick axis rests at the center.
float ReadAxisRange(const Source& source, const Reading& reading, float restValue, float endValue)
{
    if (source.SourceKind == Source::Kind::None)
        return 0.0f;
    const SourceReading value = ReadSource(source, reading);
    if (value.Digital)
        return value.Fraction * endValue;
    return restValue + value.Fraction * (endValue - restValue);
}

bool IsTrigger(int axis)
{
    return axis == static_cast<int>(GamepadAxis::LeftTrigger) || axis == static_cast<int>(GamepadAxis::RightTrigger);
}

bool IsStickY(int axis)
{
    return axis == static_cast<int>(GamepadAxis::LeftY) || axis == static_cast<int>(GamepadAxis::RightY);
}

} // namespace

void ControllerMapping::Apply(const Reading& reading, GamepadFrame::Slot& slot) const
{
    slot.connected = true;
    for (int button = 0; button < kGamepadButtonCount; ++button)
        slot.buttons[button] = ReadButton(Buttons[button], reading);

    for (int axis = 0; axis < kGamepadAxisCount; ++axis)
    {
        // A trigger reads 0 to 1; a stick reads -1 to 1.
        const float restValue = IsTrigger(axis) ? 0.0f : -1.0f;
        float value = 0.0f;
        if (Axes[axis].SourceKind != Source::Kind::None)
            value = ReadAxisRange(Axes[axis], reading, restValue, 1.0f);
        else
            value = ReadAxisRange(PositiveHalfAxes[axis], reading, 0.0f, 1.0f) +
                    ReadAxisRange(NegativeHalfAxes[axis], reading, 0.0f, -1.0f);
        value = std::clamp(value, restValue, 1.0f);
        // The database reads a stick +Y down; GamepadAxis reads it +Y up.
        slot.axes[axis] = IsStickY(axis) ? -value : value;
    }
}

} // namespace GameEngine::Input
