#pragma once

#include "Rendering/Sky/SolarPath.h"

#include <cstdint>
#include <string>

namespace GameEngine::Components
{
struct SkyEnvironment;
}

namespace GameEngine::Editor
{

/// Which axis a Day Cycle track's graph runs along. Either way the track prints the other axis
/// under every tick: the hours of the scene's path under the degrees, or the degrees under the
/// hours. Keys are edited in the elevation view; the time-of-day view draws them through the path.
enum class DayCycleAxisView : std::uint8_t
{
    Elevation,
    TimeOfDay,
};

/// The elevation view's lowest degree: the night past astronomical twilight. Its highest is the open
/// scene's noon, rounded up to the next step (DayCycleTrackRows).
inline constexpr float kDayCycleAxisLowestDegrees = -24.0f;
/// The elevation view's tick step above the horizon, and the zenith the axis never passes.
inline constexpr float kDayAxisStepDegrees = 10.0f;
inline constexpr float kDayCycleAxisZenithDegrees = 90.0f;
/// The twilight band drawn on the graphs: the sun from -18 degrees to the horizon.
inline constexpr float kDayCycleTwilightLowestDegrees = -18.0f;
inline constexpr float kHoursPerDay = 24.0f;

/// The axis that leads in every Day Cycle surface (the window and the profile's inspector) for the
/// editor session: the toggle on either sets it for both, and a reopened window keeps it.
DayCycleAxisView& DayCycleSessionAxisView();

/// The sun's path of `sky`: what puts the hours of the open scene beside the degrees.
Rendering::SolarFrame DayCyclePathOf(const Components::SkyEnvironment& sky);

/// The sun's elevation in degrees at `hours` of local solar time on `path`.
float ElevationDegreesAtHour(const Rendering::SolarFrame& path, float hours);

/// Whether the sun at `hours` stands on the setting half of its day (its bearing west of the
/// meridian), the half a separate dusk keys.
bool IsDuskHour(const Rendering::SolarFrame& path, float hours);

/// The hour at which the sun stands `degrees` high on the setting half (noon to midnight) when
/// `duskHalf`, else on the rising half (midnight to noon). A height the path never reaches gives
/// the nearest end of the half: noon for one above the path, midnight for one below.
float HourAtElevation(const Rendering::SolarFrame& path, float degrees, bool duskHalf);

/// "-6°", the elevation to the degree, as the axis prints it.
std::string DayCycleDegreesTickText(float degrees);
/// "-5.7°", the elevation to a tenth of a degree, as the playhead prints it.
std::string DayCycleDegreesText(float degrees);

} // namespace GameEngine::Editor
