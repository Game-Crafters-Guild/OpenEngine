#include "DayCycle/DayCycleAxis.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"

#include <cmath>
#include <cstdio>

namespace GameEngine::Editor
{
namespace
{
constexpr float kNoonHours = 12.0f;
constexpr float kHalfOfDayDegrees = 180.0f;
// The step the half day is searched in for the crossing, then halved down to a second.
constexpr float kSearchStepHours = 0.25f;
constexpr int kBisections = 16;
} // namespace

DayCycleAxisView& DayCycleSessionAxisView()
{
    static DayCycleAxisView view = DayCycleAxisView::Elevation;
    return view;
}

Rendering::SolarFrame DayCyclePathOf(const Components::SkyEnvironment& sky)
{
    return Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(sky));
}

float ElevationDegreesAtHour(const Rendering::SolarFrame& path, float hours)
{
    return Rendering::SolarPositionAtHour(path, hours).ElevationDegrees;
}

bool IsDuskHour(const Rendering::SolarFrame& path, float hours)
{
    return Rendering::SolarPositionAtHour(path, hours).AzimuthDegrees > kHalfOfDayDegrees;
}

float HourAtElevation(const Rendering::SolarFrame& path, float degrees, bool duskHalf)
{
    // From noon toward midnight on the setting half, toward the previous midnight on the rising
    // half: the sun only sinks along either walk, so the first sample at or below the height
    // brackets the crossing.
    const float direction = duskHalf ? 1.0f : -1.0f;
    const float midnight = duskHalf ? kHoursPerDay : 0.0f;
    if (ElevationDegreesAtHour(path, kNoonHours) <= degrees)
        return kNoonHours;
    float above = kNoonHours;
    for (float offset = kSearchStepHours; offset <= kNoonHours + 1e-3f; offset += kSearchStepHours)
    {
        const float hours = kNoonHours + direction * offset;
        if (ElevationDegreesAtHour(path, hours) > degrees)
        {
            above = hours;
            continue;
        }
        float below = hours;
        for (int i = 0; i < kBisections; ++i)
        {
            const float middle = 0.5f * (above + below);
            (ElevationDegreesAtHour(path, middle) > degrees ? above : below) = middle;
        }
        return 0.5f * (above + below);
    }
    return midnight;
}

std::string DayCycleDegreesTickText(float degrees)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%+.0f\xC2\xB0", static_cast<double>(degrees));
    return std::lround(degrees) == 0 ? std::string("0\xC2\xB0") : std::string(text);
}

std::string DayCycleDegreesText(float degrees)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f\xC2\xB0", static_cast<double>(degrees));
    return text;
}

} // namespace GameEngine::Editor
