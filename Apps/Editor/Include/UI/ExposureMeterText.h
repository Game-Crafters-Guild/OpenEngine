#pragma once

#include "Components/Rendering/PostProcessVolume.h" // ExposureMeter

namespace GameEngine::Editor
{

// The user-facing name of each Auto meter (Components::ExposureMeter), shared by the Camera
// inspector and the Scene View's popover, menu and Preferences page.
inline constexpr const char* kExposureMeterPictureLabel = "Picture";
inline constexpr const char* kExposureMeterLightsLabel = "Lights";

inline const char* ExposureMeterLabel(Components::ExposureMeter meter)
{
    return meter == Components::ExposureMeter::Lights ? kExposureMeterLightsLabel : kExposureMeterPictureLabel;
}

// The tooltip of every Meter row: what each meter reads.
inline constexpr const char* kExposureMeterTooltip =
    "Picture: meters what the camera sees and adapts over time, like an eye.\n"
    "Lights: reads the light falling on the world; steady, follows the time of day, never hunts.";

} // namespace GameEngine::Editor
