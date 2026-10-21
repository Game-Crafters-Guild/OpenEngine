#pragma once

#include <cstdint>

namespace GameEngine
{
class Button;
}

namespace GameEngine::Editor
{
enum class PlayModeState : std::uint8_t;

/// The top toolbar Play button's class while play mode runs (the green play icon).
/// It is a class of its own, not Button's `pressed`: Button adds and removes `pressed`
/// on every left click, and the Play button stays clickable during play so its
/// right-click menu is reachable.
inline constexpr const char* kPlayButtonActiveClass = "toolbar-play-active";

/// Applies play mode `state` to the Play button: clickable in Edit, Play and Paused (the
/// click handler acts only in Edit; the other two keep its menu reachable), disabled in
/// review and transitions, and kPlayButtonActiveClass while play mode is entering,
/// running, paused or exiting.
void ApplyPlayButtonState(Button& playButton, PlayModeState state);

} // namespace GameEngine::Editor
