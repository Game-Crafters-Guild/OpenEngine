#pragma once

#include "UI/Interaction/ContextMenuManipulator.h"

#include <functional>
#include <vector>

namespace GameEngine::Audio
{
class AudioSystem;
}

namespace GameEngine::Editor
{

/**
 * Item set for the top toolbar Play button's right-click menu: a checkable "Audio Enabled"
 * row, checked by default, that toggles AudioEnabledSettings. The check mark resolves
 * from `audio` at every show. `onAudioEnabledChanged` runs after each toggle with the new
 * state, so the toolbar can update its muted cue. `audio` may be null when audio failed
 * to start; it must outlive the menu (the engine owns it for the editor's lifetime).
 */
std::vector<ContextMenuManipulator::Item> PlayButtonMenuItems(
    Audio::AudioSystem* audio, std::function<void(bool enabled)> onAudioEnabledChanged);

} // namespace GameEngine::Editor
