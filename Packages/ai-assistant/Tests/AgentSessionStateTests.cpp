#include "AgentSessionState.h"
#include "AiAssistantSettings.h"
#include "Providers/IAgentProvider.h"
#include "ScopedPreferencesRoot.h"

#include <gtest/gtest.h>

#include <memory>

namespace GameEngine
{
TEST(AgentSessionStateTests, ATurnOfAReplacedProviderCannotRecordItsSessionIntoTheNewConnection)
{
    ScopedPreferencesRoot preferences;
    AgentSessionState& state = AgentSessionState::Get();

    AiAssistantSettings::SetClaudeExecutable("C:/Tools/old/claude.exe");
    const std::shared_ptr<IAgentProvider> replaced = state.Provider("claude-session");
    AiAssistantSettings::SetClaudeExecutable("C:/Tools/new/claude.exe");
    const std::shared_ptr<IAgentProvider> current = state.Provider("claude-session");
    ASSERT_NE(replaced, current);

    state.SetSessionId(*replaced, "old-session");
    EXPECT_EQ(state.SessionId("claude-session"), "");
    state.SetSessionId(*current, "new-session");
    EXPECT_EQ(state.SessionId("claude-session"), "new-session");
}
} // namespace GameEngine
