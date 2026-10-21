#include "PlayMode/PlayButtonMenu.h"

#include "Editor/Settings/AudioEnabledSettings.h"
#include "UI/EditorIcons.h"

#include <utility>

namespace GameEngine::Editor
{

namespace
{
/// Title of the checkable audio row in the Play button's context menu.
constexpr const char* kPlayButtonAudioEnabledItem = "Audio Enabled";
} // namespace

std::vector<ContextMenuManipulator::Item> PlayButtonMenuItems(
    Audio::AudioSystem* audio, std::function<void(bool enabled)> onAudioEnabledChanged)
{
    std::vector<ContextMenuManipulator::Item> items;
    items.push_back({.Path = kPlayButtonAudioEnabledItem,
                     .IconPath = EditorIcons::kMusicNote,
                     .OnActivate =
                         [audio, onChanged = std::move(onAudioEnabledChanged)]
                         {
                             const bool enabled = AudioEnabledSettings::Toggle(audio);
                             if (onChanged)
                                 onChanged(enabled);
                         },
                     .State = [audio]
                     { return ContextMenuManipulator::ItemState{.Checked = AudioEnabledSettings::IsEnabled(audio)}; }});
    return items;
}

} // namespace GameEngine::Editor
