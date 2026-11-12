#include "AiAssistantSettings.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "ScopedEnvironmentVariable.h"
#include "ScopedPreferencesRoot.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
// The setup page asks for the assistant, then the method: Claude has two methods, Codex one,
// and every connection's name reads "Assistant (Method)".
TEST(AiAssistantSettingsTests, EachAssistantOffersItsMethodsUnderAssistantMethodNames)
{
    std::vector<std::string_view> assistants;
    for (const AiAssistantSettings::AssistantChoice& choice : AiAssistantSettings::AssistantChoices())
        assistants.push_back(choice.Name);
    EXPECT_EQ(assistants, (std::vector<std::string_view>{"Claude", "Codex"}));

    const std::vector<AiAssistantSettings::ProviderChoice> claude = AiAssistantSettings::MethodChoices("Claude");
    ASSERT_EQ(claude.size(), 2u);
    EXPECT_EQ(claude[0].Id, "claude-session") << "Subscription first: choosing Claude selects it";
    EXPECT_EQ(claude[1].Id, "claude-api");
    ASSERT_EQ(AiAssistantSettings::MethodChoices("Codex").size(), 1u);
    EXPECT_TRUE(AiAssistantSettings::MethodChoices("Local LLM").empty());

    for (const AiAssistantSettings::ProviderChoice& choice : AiAssistantSettings::ProviderChoices())
    {
        EXPECT_EQ(choice.Label, std::string(choice.Assistant) + " (" + std::string(choice.Method) + ")");
        EXPECT_EQ(AiAssistantSettings::ProviderChoiceOf(choice.Id).Id, choice.Id);
    }
}

// One stored value holds both choices: choosing an assistant stores its first method, and an
// assistant without a connection changes nothing.
TEST(AiAssistantSettingsTests, ChoosingAnAssistantStoresItsFirstMethod)
{
    ScopedPreferencesRoot preferences;
    AiAssistantSettings::SetProvider("claude-api");
    EXPECT_EQ(AiAssistantSettings::ProviderChoiceOf(AiAssistantSettings::Provider()).Assistant, "Claude");

    AiAssistantSettings::SetAssistant("Claude");
    EXPECT_EQ(AiAssistantSettings::Provider(), "claude-api") << "the shown assistant keeps its method";
    AiAssistantSettings::SetAssistant("Codex");
    EXPECT_EQ(AiAssistantSettings::Provider(), "codex-session");
    const uint64_t generation = AiAssistantSettings::ProviderGeneration();
    AiAssistantSettings::SetAssistant("Local LLM");
    EXPECT_EQ(AiAssistantSettings::Provider(), "codex-session");
    EXPECT_EQ(AiAssistantSettings::ProviderGeneration(), generation);
    AiAssistantSettings::SetAssistant("Claude");
    EXPECT_EQ(AiAssistantSettings::Provider(), "claude-session");
}

// The page's rows live in its layout, so the category's search words name them: a search for
// a row's label still finds the page.
TEST(AiAssistantSettingsTests, TheSearchWordsNameEveryRowOfThePage)
{
    const Editor::SettingsCategoryDescriptor category = AiAssistantSettings::BuildCategory();
    EXPECT_TRUE(category.CreateContent);
    for (const char* row : {"claude code executable", "codex executable", "node executable", "model", "effort",
                            "mode for new conversations", "hold the microphone to speak",
                            "send when the microphone stops"})
        EXPECT_NE(category.SearchKeywords.find(row), std::string::npos) << row;
}

// An executable changed on the page moves the setup generation once, so the page and the
// panel check again; storing the same path again moves nothing.
TEST(AiAssistantSettingsTests, ANewExecutableAsksForANewCheck)
{
    ScopedPreferencesRoot preferences;
    AiAssistantSettings::SetClaudeExecutable("C:/Tools/claude.exe");
    const uint64_t generation = AiAssistantSettings::SetupGeneration();
    AiAssistantSettings::SetClaudeExecutable("C:/Tools/claude.exe");
    EXPECT_EQ(AiAssistantSettings::SetupGeneration(), generation);
    AiAssistantSettings::SetNodeExecutable("C:/Tools/node.exe");
    EXPECT_EQ(AiAssistantSettings::SetupGeneration(), generation + 1);
    EXPECT_EQ(AiAssistantSettings::NodeExecutable(), "C:/Tools/node.exe");
    AiAssistantSettings::SetCodexExecutable("C:/Tools/codex.exe");
    EXPECT_EQ(AiAssistantSettings::SetupGeneration(), generation + 2);
    EXPECT_EQ(AiAssistantSettings::CodexExecutable(), "C:/Tools/codex.exe");
}

// An executable never set or cleared is the program's name, searched for on PATH.
TEST(AiAssistantSettingsTests, AnEmptyExecutableIsTheProgramOnPath)
{
    ScopedPreferencesRoot preferences;
    EXPECT_EQ(AiAssistantSettings::CodexExecutable(), CodexSessionProvider::kDefaultExecutable);
    AiAssistantSettings::SetCodexExecutable("C:/Tools/codex.exe");
    AiAssistantSettings::SetCodexExecutable("");
    EXPECT_EQ(AiAssistantSettings::CodexExecutable(), CodexSessionProvider::kDefaultExecutable);
    AiAssistantSettings::SetClaudeExecutable("");
    EXPECT_EQ(AiAssistantSettings::ClaudeExecutable(), ClaudeSessionProvider::kDefaultExecutable);
    AiAssistantSettings::SetNodeExecutable("C:/Tools/node.exe");
    AiAssistantSettings::SetNodeExecutable("");
    EXPECT_EQ(AiAssistantSettings::NodeExecutable(), "node");
}

// The subscription's needs line names the oldest Claude Code a turn works with.
TEST(AiAssistantSettingsTests, TheClaudeSubscriptionNeedsLineNamesTheMinimumVersion)
{
    const auto& version = ClaudeSessionProvider::kMinimumVersion;
    const std::string minimum = std::format("Claude Code {}.{}.{} or later", version[0], version[1], version[2]);
    EXPECT_NE(AiAssistantSettings::ProviderChoiceOf(ClaudeSessionProvider::kId).MethodNeeds.find(minimum),
              std::string_view::npos);
}

TEST(AiAssistantSettingsTests, TheConnectionPersistsAndPreferencesNeverHoldTheKey)
{
    ScopedPreferencesRoot preferences;
    ScopedEnvironmentVariable key("ANTHROPIC_API_KEY", "sk-ant-fixture-not-a-key");

    AiAssistantSettings::SetProvider("claude-session");
    const uint64_t generation = AiAssistantSettings::ProviderGeneration();
    AiAssistantSettings::SetProvider("codex-session");
    EXPECT_EQ(AiAssistantSettings::Provider(), "codex-session");
    EXPECT_EQ(AiAssistantSettings::ProviderGeneration(), generation + 1);
    AiAssistantSettings::SetProvider("codex-session");
    AiAssistantSettings::SetProvider("not-a-provider");
    EXPECT_EQ(AiAssistantSettings::Provider(), "codex-session");
    EXPECT_EQ(AiAssistantSettings::ProviderGeneration(), generation + 1);

    const std::filesystem::path file = Editor::OpenEditorPreferences().GetFilePath();
    std::ifstream stream(file, std::ios::binary);
    ASSERT_TRUE(stream) << file;
    const std::string contents(std::istreambuf_iterator<char>(stream), {});
    EXPECT_NE(contents.find("aiAssistant.provider"), std::string::npos);
    EXPECT_EQ(contents.find("sk-ant-"), std::string::npos);
}

// A Claude session turn always names its model and effort: opus at high until the user
// chooses, and the first model or the initial effort when the stored one is no longer
// offered.
TEST(AiAssistantSettingsTests, AClaudeSessionAlwaysAsksForAnOfferedModelAndEffort)
{
    ScopedPreferencesRoot preferences;
    EXPECT_EQ(AiAssistantSettings::Model("claude-session"), "opus");
    EXPECT_EQ(AiAssistantSettings::Effort("claude-session"), "high");

    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    prefs.Load();
    prefs.SetString("aiAssistant.claudeSessionModel", "default");
    prefs.SetString("aiAssistant.claudeSessionEffort", "default");
    prefs.Save();
    EXPECT_EQ(AiAssistantSettings::Model("claude-session"), "opus");
    EXPECT_EQ(AiAssistantSettings::Effort("claude-session"), "high");
    EXPECT_EQ(AiAssistantSettings::Effort("claude-api"), "") << "a connection without effort levels asks for none";
}

// The last session is kept per connection and per project, in the project's per-user
// settings; an empty id removes it.
TEST(AiAssistantSettingsTests, TheLastSessionIsStoredPerConnectionAndProject)
{
    ScopedPreferencesRoot preferences;
    const std::filesystem::path game = std::filesystem::temp_directory_path() / "AiAssistantSettingsTests-game";
    const std::filesystem::path other = std::filesystem::temp_directory_path() / "AiAssistantSettingsTests-other";

    AiAssistantSettings::SetLastSession("claude-session", game, "claude-id");
    AiAssistantSettings::SetLastSession("codex-session", game, "codex-id");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", game), "claude-id");
    EXPECT_EQ(AiAssistantSettings::LastSession("codex-session", game), "codex-id");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", other), "");

    const std::filesystem::path file = Editor::OpenUserProjectSettings(game).GetFilePath();
    std::ifstream stream(file, std::ios::binary);
    const std::string contents(std::istreambuf_iterator<char>(stream), {});
    EXPECT_NE(contents.find("\"aiAssistant.session.claude-session\""), std::string::npos) << file;
    stream.close();

    AiAssistantSettings::SetLastSession("claude-session", game, "");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", game), "");
    EXPECT_EQ(AiAssistantSettings::LastSession("codex-session", game), "codex-id");
    AiAssistantSettings::SetLastSession("claude-session", {}, "ignored");
    EXPECT_EQ(AiAssistantSettings::LastSession("claude-session", {}), "");
}
} // namespace GameEngine
