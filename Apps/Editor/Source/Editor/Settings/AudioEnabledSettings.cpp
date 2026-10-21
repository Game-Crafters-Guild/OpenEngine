#include "Editor/Settings/AudioEnabledSettings.h"

#include "Audio/AudioSystem.h"
#include "Editor/Settings/SettingsStore.h"

#include <string>

namespace GameEngine::Editor
{

namespace
{
/// Editor preferences key (Preferences.json) holding whether the editor plays audio.
constexpr const char* kAudioEnabledPreferenceKey = "audio.enabled";

bool LoadSavedEnabled()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    (void)prefs.Load(&error);
    bool enabled = true;
    prefs.TryGetBool(kAudioEnabledPreferenceKey, enabled);
    return enabled;
}
} // namespace

bool AudioEnabledSettings::ApplySaved(Audio::AudioSystem* audio)
{
    const bool enabled = LoadSavedEnabled();
    if (audio)
        audio->SetMasterMuted(!enabled);
    return enabled;
}

void AudioEnabledSettings::Set(Audio::AudioSystem* audio, bool enabled)
{
    if (audio)
        audio->SetMasterMuted(!enabled);

    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    (void)prefs.Load(&error);
    prefs.SetBool(kAudioEnabledPreferenceKey, enabled);
    (void)prefs.Save(&error);
}

bool AudioEnabledSettings::Toggle(Audio::AudioSystem* audio)
{
    const bool enabled = !IsEnabled(audio);
    Set(audio, enabled);
    return enabled;
}

bool AudioEnabledSettings::IsEnabled(const Audio::AudioSystem* audio)
{
    return audio ? !audio->IsMasterMuted() : LoadSavedEnabled();
}

} // namespace GameEngine::Editor
