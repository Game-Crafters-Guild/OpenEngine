#include "PlayMode/AudioToggleButton.h"

#include "Editor/Settings/AudioEnabledSettings.h"
#include "UI/Controls/Button.h"
#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{
constexpr const char* kAudioEnabledTooltip = "Audio enabled. Click to mute.";
constexpr const char* kAudioMutedTooltip = "Audio muted. Click to enable.";
} // namespace

void ApplyAudioToggleState(Button& button, bool enabled)
{
    if (enabled)
    {
        button.RemoveClass(kAudioMutedSpeakerIconClass);
        button.AddClass(kAudioOnSpeakerIconClass);
    }
    else
    {
        button.RemoveClass(kAudioOnSpeakerIconClass);
        button.AddClass(kAudioMutedSpeakerIconClass);
    }
    button.SetTooltip(enabled ? kAudioEnabledTooltip : kAudioMutedTooltip);
}

void WireAudioToggleButton(Button& button, Audio::AudioSystem* audio,
                           std::function<void(bool enabled)> onAudioEnabledChanged)
{
    ApplyAudioToggleState(button, AudioEnabledSettings::IsEnabled(audio));
    button.RegisterEventHandler(kEventButtonClick,
                                [&button, audio, onChanged = std::move(onAudioEnabledChanged)](UIEvent&)
                                {
                                    const bool enabled = AudioEnabledSettings::Toggle(audio);
                                    ApplyAudioToggleState(button, enabled);
                                    if (onChanged)
                                        onChanged(enabled);
                                });
}

} // namespace GameEngine::Editor
