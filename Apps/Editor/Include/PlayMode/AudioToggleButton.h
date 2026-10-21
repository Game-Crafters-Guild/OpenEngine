#pragma once

#include <functional>

namespace GameEngine
{
class Button;
}

namespace GameEngine::Audio
{
class AudioSystem;
}

namespace GameEngine::Editor
{

/// Icon class of the top toolbar's audio toggle while the editor plays audio (a speaker
/// with sound waves, `editor:Icons/audio-on-speaker.svg`, EditorTopToolbar.css).
inline constexpr const char* kAudioOnSpeakerIconClass = "audio-on-speaker-icon";

/// Icon class of the top toolbar's audio toggle while audio is muted (a speaker with an
/// X, `editor:Icons/audio-muted-speaker.svg`, EditorTopToolbar.css).
inline constexpr const char* kAudioMutedSpeakerIconClass = "audio-muted-speaker-icon";

/// Shows `enabled` on the audio toggle right of the Play button: exactly one of the two
/// speaker icon classes, and the tooltip.
void ApplyAudioToggleState(Button& button, bool enabled);

/**
 * Wires the audio toggle right of the Play button to AudioEnabledSettings, the same state
 * as the Play button menu's "Audio Enabled" row. Shows the current state of `audio` (the
 * saved preference when `audio` is null) at once; each click toggles it, shows the new
 * state, then runs `onAudioEnabledChanged` with it so the toolbar can update its other
 * audio cues. `audio` must outlive the button (the engine owns it for the editor's
 * lifetime).
 */
void WireAudioToggleButton(Button& button, Audio::AudioSystem* audio,
                           std::function<void(bool enabled)> onAudioEnabledChanged);

} // namespace GameEngine::Editor
