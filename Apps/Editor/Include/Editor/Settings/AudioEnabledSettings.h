#pragma once

namespace GameEngine::Audio
{
class AudioSystem;
}

namespace GameEngine::Editor
{

/**
 * Whether the editor plays audio, toggled from the Play button's context menu and from the
 * audio toggle right of the Play button.
 *
 * Audio is enabled by default. Disabling it silences the speakers for every sound the
 * engine plays while the editor runs: play mode, inspector previews and editor sounds
 * alike, through AudioSystem::SetMasterMuted. Movie recordings keep their sound. The
 * choice persists in the editor preferences (`audio.enabled` in Preferences.json), so
 * the next session starts the same way.
 * Passing a null audio system (audio failed to initialize) still reads or writes the
 * preference.
 */
class AudioEnabledSettings
{
  public:
    /// Applies the saved choice to `audio` and returns it. Unset means enabled.
    static bool ApplySaved(Audio::AudioSystem* audio);

    /// Enables or mutes `audio` and saves the choice to the editor preferences.
    static void Set(Audio::AudioSystem* audio, bool enabled);

    /// Flips the state IsEnabled reads, applies it to `audio` and saves it; returns the new
    /// state. The Play button menu's row and the toolbar's audio toggle both toggle here.
    static bool Toggle(Audio::AudioSystem* audio);

    /// The live state of `audio`, or the saved preference when `audio` is null.
    static bool IsEnabled(const Audio::AudioSystem* audio);
};

} // namespace GameEngine::Editor
