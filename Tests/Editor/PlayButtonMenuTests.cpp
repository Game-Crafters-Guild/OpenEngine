// The Play button's right-click menu: its "Audio Enabled" row, checked by default,
// toggles the engine audio mute and the editor preference `audio.enabled` together,
// its check mark follows the audio system, and the saved choice is applied to the
// next session's audio system.
//
// The audio system is never initialized: the mute state lives on AudioSystem itself,
// so these tests need no audio device. Preferences go to a scratch user-data root.

#include <gtest/gtest.h>

#include "Editor/Settings/AudioEnabledSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "PlayMode/PlayButtonMenu.h"
#include "UI/EditorIcons.h"

#include "Assets/AssetManager.h"
#include "Audio/AudioSystem.h"
#include "Platform/Process.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{

using namespace GameEngine;

// The row's label and the preference key are what a user sees and what Preferences.json
// holds, so the tests spell them out rather than reading the constants back.
constexpr const char* kAudioEnabledLabel = "Audio Enabled";
constexpr const char* kAudioEnabledKey = "audio.enabled";

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

std::optional<bool> SavedAudioEnabledPreference()
{
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    std::string error;
    (void)prefs.Load(&error);
    bool enabled = true;
    if (!prefs.TryGetBool(kAudioEnabledKey, enabled))
        return std::nullopt;
    return enabled;
}

const ContextMenuManipulator::Item* FindItem(const std::vector<ContextMenuManipulator::Item>& items,
                                             const std::string& path)
{
    for (const ContextMenuManipulator::Item& item : items)
    {
        if (item.Path == path)
            return &item;
    }
    return nullptr;
}

class PlayButtonMenuTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        if (const char* previous = std::getenv("GE_EDITOR_USER_DATA_ROOT"))
            m_PreviousRoot = previous;
        // One root per process: two EditorTests runs on one machine must not delete each
        // other's preferences.
        m_Root = std::filesystem::temp_directory_path() /
                 ("PlayButtonMenuTests_user_data_" + std::to_string(Platform::GetCurrentProcessId()));
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

TEST_F(PlayButtonMenuTest, AudioEnabledItemIsCheckedOnAFreshProfile)
{
    Audio::AudioSystem audio(m_Assets);
    EXPECT_TRUE(Editor::AudioEnabledSettings::ApplySaved(&audio));
    const std::vector<ContextMenuManipulator::Item> items = Editor::PlayButtonMenuItems(&audio, {});

    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].Path, kAudioEnabledLabel);
    EXPECT_EQ(items[0].IconPath, EditorIcons::kAudioEnabled);
    ASSERT_TRUE(items[0].State);
    EXPECT_TRUE(items[0].State().Enabled);
    EXPECT_TRUE(items[0].State().Checked) << "audio is enabled until the user turns it off";
    EXPECT_FALSE(audio.IsMasterMuted());
    EXPECT_FALSE(SavedAudioEnabledPreference().has_value()) << "a fresh profile saves nothing";
}

TEST_F(PlayButtonMenuTest, AudioEnabledItemTogglesTheAudioSystemAndThePreference)
{
    Audio::AudioSystem audio(m_Assets);
    std::vector<bool> reported;
    const std::vector<ContextMenuManipulator::Item> items =
        Editor::PlayButtonMenuItems(&audio, [&reported](bool enabled) { reported.push_back(enabled); });

    const ContextMenuManipulator::Item* item = FindItem(items, kAudioEnabledLabel);
    ASSERT_NE(item, nullptr);
    ASSERT_TRUE(item->OnActivate);
    ASSERT_TRUE(item->State);

    item->OnActivate();
    EXPECT_TRUE(audio.IsMasterMuted());
    EXPECT_EQ(SavedAudioEnabledPreference(), std::optional<bool>(false));
    EXPECT_FALSE(item->State().Checked);
    const auto mutedMenu = Editor::PlayButtonMenuItems(&audio, {});
    ASSERT_EQ(mutedMenu.size(), 1u);
    EXPECT_EQ(mutedMenu[0].IconPath, EditorIcons::kAudioMuted);
    EXPECT_TRUE(mutedMenu[0].State().Enabled) << "muted audio remains available to unmute";

    item->OnActivate();
    EXPECT_FALSE(audio.IsMasterMuted());
    EXPECT_EQ(SavedAudioEnabledPreference(), std::optional<bool>(true));
    EXPECT_TRUE(item->State().Checked);
    const auto enabledMenu = Editor::PlayButtonMenuItems(&audio, {});
    ASSERT_EQ(enabledMenu.size(), 1u);
    EXPECT_EQ(enabledMenu[0].IconPath, EditorIcons::kAudioEnabled);
    EXPECT_TRUE(enabledMenu[0].State().Enabled);

    EXPECT_EQ(reported, (std::vector<bool>{false, true})) << "the toolbar tooltip hears every toggle";
}

TEST_F(PlayButtonMenuTest, SavedDisabledAudioIsAppliedToTheNextSessionsAudioSystem)
{
    {
        Audio::AudioSystem firstSession(m_Assets);
        Editor::AudioEnabledSettings::Set(&firstSession, false);
    }

    Audio::AudioSystem nextSession(m_Assets);
    ASSERT_FALSE(nextSession.IsMasterMuted());
    EXPECT_FALSE(Editor::AudioEnabledSettings::ApplySaved(&nextSession));
    EXPECT_TRUE(nextSession.IsMasterMuted());
    const auto items = Editor::PlayButtonMenuItems(&nextSession, {});
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].IconPath, EditorIcons::kAudioMuted);
    EXPECT_FALSE(items[0].State().Checked);
    EXPECT_TRUE(items[0].State().Enabled);
}

TEST_F(PlayButtonMenuTest, UnsetPreferenceLeavesAudioEnabled)
{
    Audio::AudioSystem audio(m_Assets);
    audio.SetMasterMuted(true);
    EXPECT_TRUE(Editor::AudioEnabledSettings::ApplySaved(&audio));
    EXPECT_FALSE(audio.IsMasterMuted());
}

TEST_F(PlayButtonMenuTest, AudioEnabledItemWorksWithoutAnAudioSystem)
{
    const std::vector<ContextMenuManipulator::Item> items = Editor::PlayButtonMenuItems(nullptr, {});
    const ContextMenuManipulator::Item* item = FindItem(items, kAudioEnabledLabel);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->State().Checked);

    item->OnActivate();
    EXPECT_EQ(SavedAudioEnabledPreference(), std::optional<bool>(false));
    EXPECT_FALSE(item->State().Checked) << "with audio disabled the check mark reads the preference";
    const auto mutedMenu = Editor::PlayButtonMenuItems(nullptr, {});
    ASSERT_EQ(mutedMenu.size(), 1u);
    EXPECT_EQ(mutedMenu[0].IconPath, EditorIcons::kAudioMuted);
    EXPECT_TRUE(mutedMenu[0].State().Enabled);
}

} // namespace
