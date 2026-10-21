// The audio toggle right of the top toolbar's Play button: one state with the Play button
// menu's "Audio Enabled" row (AudioEnabledSettings). The button shows the speaker with
// sound waves while audio is enabled, the default, and the speaker with an X while muted;
// a click on either control updates the other.
//
// The audio system is never initialized: the mute state lives on AudioSystem itself, so
// these tests need no audio device. Preferences go to a scratch user-data root.

#include <gtest/gtest.h>

#include "Editor/Settings/AudioEnabledSettings.h"
#include "PlayMode/AudioToggleButton.h"
#include "PlayMode/PlayButtonMenu.h"

#include "Assets/AssetManager.h"
#include "Audio/AudioSystem.h"
#include "Platform/Process.h"
#include "UI/Controls/Button.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

using namespace GameEngine;

// The icon classes and tooltips are what the stylesheet matches and what a user reads, so
// the tests spell them out rather than reading the constants back.
constexpr const char* kOnIconClass = "audio-on-speaker-icon";
constexpr const char* kMutedIconClass = "audio-muted-speaker-icon";
constexpr const char* kEnabledTooltip = "Audio enabled. Click to mute.";
constexpr const char* kMutedTooltip = "Audio muted. Click to enable.";

void SetUserDataRoot(const std::string& root)
{
#ifdef _WIN32
    _putenv_s("GE_EDITOR_USER_DATA_ROOT", root.c_str());
#else
    if (root.empty())
        unsetenv("GE_EDITOR_USER_DATA_ROOT");
    else
        setenv("GE_EDITOR_USER_DATA_ROOT", root.c_str(), 1);
#endif
}

void ExpectShowsEnabled(const Button& button)
{
    EXPECT_TRUE(button.HasClass(kOnIconClass));
    EXPECT_FALSE(button.HasClass(kMutedIconClass));
    EXPECT_EQ(button.GetTooltip(), kEnabledTooltip);
}

void ExpectShowsMuted(const Button& button)
{
    EXPECT_TRUE(button.HasClass(kMutedIconClass));
    EXPECT_FALSE(button.HasClass(kOnIconClass));
    EXPECT_EQ(button.GetTooltip(), kMutedTooltip);
}

class AudioToggleButtonTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (const char* previous = std::getenv("GE_EDITOR_USER_DATA_ROOT"))
            m_PreviousRoot = previous;
        // One root per process: two EditorTests runs on one machine must not delete each
        // other's preferences.
        m_Root = std::filesystem::temp_directory_path() /
                 ("AudioToggleButtonTests_user_data_" + std::to_string(Platform::GetCurrentProcessId()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
        SetUserDataRoot(m_Root.string());
    }

    void TearDown() override
    {
        SetUserDataRoot(m_PreviousRoot);
        std::error_code ignored;
        std::filesystem::remove_all(m_Root, ignored);
    }

    AssetManager m_Assets;
    std::filesystem::path m_Root;
    std::string m_PreviousRoot;
};

TEST_F(AudioToggleButtonTest, ShowsAudioEnabledOnAFreshProfile)
{
    Audio::AudioSystem audio(m_Assets);
    ASSERT_TRUE(Editor::AudioEnabledSettings::ApplySaved(&audio));
    Button button;
    Editor::WireAudioToggleButton(button, &audio, {});

    ExpectShowsEnabled(button);
}

TEST_F(AudioToggleButtonTest, ShowsASavedMuteAtStartup)
{
    {
        Audio::AudioSystem firstSession(m_Assets);
        Editor::AudioEnabledSettings::Set(&firstSession, false);
    }

    Audio::AudioSystem nextSession(m_Assets);
    ASSERT_FALSE(Editor::AudioEnabledSettings::ApplySaved(&nextSession));
    Button button;
    Editor::WireAudioToggleButton(button, &nextSession, {});

    ExpectShowsMuted(button);
}

TEST_F(AudioToggleButtonTest, AClickTogglesTheAudioAndTheMenuRowFollows)
{
    Audio::AudioSystem audio(m_Assets);
    std::vector<bool> reported;
    Button button;
    Editor::WireAudioToggleButton(button, &audio, [&reported](bool enabled) { reported.push_back(enabled); });
    const std::vector<ContextMenuManipulator::Item> menu = Editor::PlayButtonMenuItems(&audio, {});
    ASSERT_EQ(menu.size(), 1u);

    button.TriggerClick();
    EXPECT_TRUE(audio.IsMasterMuted());
    EXPECT_FALSE(Editor::AudioEnabledSettings::IsEnabled(nullptr)) << "the click saves the preference";
    EXPECT_FALSE(menu[0].State().Checked);
    ExpectShowsMuted(button);

    button.TriggerClick();
    EXPECT_FALSE(audio.IsMasterMuted());
    EXPECT_TRUE(Editor::AudioEnabledSettings::IsEnabled(nullptr));
    EXPECT_TRUE(menu[0].State().Checked);
    ExpectShowsEnabled(button);

    EXPECT_EQ(reported, (std::vector<bool>{false, true})) << "the Play button's tooltip hears every click";
}

TEST_F(AudioToggleButtonTest, TheMenuRowUpdatesTheButton)
{
    Audio::AudioSystem audio(m_Assets);
    Button button;
    Editor::WireAudioToggleButton(button, &audio, {});
    // The toolbar routes the row's change to the button, as EditorTopToolbar::SetAudioEnabled does.
    const std::vector<ContextMenuManipulator::Item> menu = Editor::PlayButtonMenuItems(
        &audio, [&button](bool enabled) { Editor::ApplyAudioToggleState(button, enabled); });
    ASSERT_EQ(menu.size(), 1u);

    menu[0].OnActivate();
    ExpectShowsMuted(button);

    menu[0].OnActivate();
    ExpectShowsEnabled(button);
}

} // namespace
