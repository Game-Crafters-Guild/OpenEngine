#include "Providers/CancelToken.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "RecordingTurnEvents.h"
#include "ScopedEnvironmentVariable.h"

#include "LogCapture.h"
#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace GameEngine
{
namespace
{
using namespace std::chrono_literals;

// The redacted recordings (one real stdout stream of each CLI, session ids replaced,
// paths and account data removed) and the fake CLI that replays them are staged
// beside this executable.
std::filesystem::path TestDirectory()
{
    return Platform::GetExecutablePath().parent_path();
}

std::string FakeCli()
{
#if defined(_WIN32)
    return (TestDirectory() / "FakeAgentCli.exe").string();
#else
    return (TestDirectory() / "FakeAgentCli").string();
#endif
}

std::string Fixture(const char* name)
{
    return (TestDirectory() / "AgentCliFixtures" / name).string();
}

AgentTurnRequest Request(std::string userText)
{
    AgentTurnRequest request;
    request.UserText = std::move(userText);
    return request;
}

constexpr const char* kFixtureSession = "00000000-0000-4000-8000-000000000001";

// The program each fix names, by its file name on this platform.
#if defined(_WIN32)
const std::string kClaudeToPick = "Claude Code's claude.exe";
const std::string kCodexToPick = "codex.exe (the native Codex program, not npm's codex.cmd)";
#else
const std::string kClaudeToPick = "Claude Code's claude program";
const std::string kCodexToPick = "the codex program";
#endif

// The panel's fix for a wrong or missing Claude Code: the place of the field first.
const std::string kClaudePanelFix = "The Claude Code executable field is in Settings > AI Assistant: pick " +
                                    kClaudeToPick + " there, or install it from claude.com/claude-code.";
} // namespace

TEST(CliSessionProviderTests, ClaudeTurnArgumentsAreTheDesignCommandLine)
{
    const ClaudeSessionProvider provider("claude", {});
    AgentTurnRequest request = Request("hello");
    request.SessionId = "session-1";
    request.Model = "opus";
    request.Effort = "high";
    request.SystemPrompt = "Be brief.";

    const std::vector<std::string> expected = {
        "-p", "--output-format", "stream-json", "--verbose", "--include-partial-messages", "--resume", "session-1",
        "--permission-prompts", "none", "--tools", "", "--strict-mcp-config", "--system-prompt-snapshot", "off",
        "--model", "opus", "--effort", "high", "--append-system-prompt", "Be brief."};
    EXPECT_EQ(provider.TurnArguments(request), expected);
}

TEST(CliSessionProviderTests, CodexTurnArgumentsPutTheExecOptionsBeforeResume)
{
    const CodexSessionProvider provider("codex", "C:/Project");
    AgentTurnRequest request = Request("hello");
    request.SessionId = "thread-1";

    const std::vector<std::string> expected = {
        "exec", "--json", "--skip-git-repo-check", "-C", std::filesystem::path("C:/Project").string(),
        "-s", "read-only", "resume", "thread-1", "-"};
    EXPECT_EQ(provider.TurnArguments(request), expected);
}

TEST(CliSessionProviderTests, RecordedClaudeTurnYieldsTheSessionIdTheDeltasInOrderAndTheAccounting)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-turn.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("Describe blue in two sentences."), events, cancel);

    const std::string reply = "Blue is the color of a clear daytime sky and deep ocean water. It is often "
                              "associated with calm, trust, and stability.";
    const std::vector<std::string> expected = {
        std::string("session:") + kFixtureSession, "delta:Blue is the color of a", "delta: clear da",
        "delta:ytime sky and de", "delta:ep ocean water", "delta:. It is", "delta: often associ",
        "delta:ated with calm, tr", "delta:ust, and stability.", "finished:" + reply};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.Model, "claude-fable-5-1") << "the model the init record names";
    EXPECT_DOUBLE_EQ(events.Result.CostUsd, 0.08598599999999999);
    EXPECT_EQ(events.Result.ModelRoundTrips, 1u);
    EXPECT_EQ(events.Result.InputTokens, 2u + 4189u + 544u);
    EXPECT_EQ(events.Result.OutputTokens, 41u);
    EXPECT_EQ(events.Result.PermissionDenials, 0u);
}

TEST(CliSessionProviderTests, RecordedCodexTurnYieldsTheThreadIdTheMessageAndTheUsageAndSkipsWarnings)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("codex-turn.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("Describe blue in two sentences."), events, cancel);

    const std::string reply = "Blue is the color of a clear sky. It often feels calm and peaceful.";
    const std::vector<std::string> expected = {std::string("session:") + kFixtureSession, "delta:" + reply,
                                               "finished:" + reply};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.InputTokens, 15868u);
    EXPECT_EQ(events.Result.OutputTokens, 63u);
}

// The recorded answer of `claude -p --resume <unknown id>`: no init, a result with no
// text and the message in `errors`.
TEST(CliSessionProviderTests, AnUnknownClaudeSessionFailsTheTurnWithTheClisMessage)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-dead-session.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Log, std::vector<std::string>{"finished:"}) << "no session is reported";
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code reported an error: No conversation found with session ID: "
                                   "00000000-0000-4000-8000-000000000009");
    EXPECT_TRUE(events.Result.SessionNotFound);
}

// The recorded answer of `claude -p --model not-a-model` (claude 2.1.296): an init, the
// error as a synthetic assistant message and a failed result carrying the same text.
TEST(CliSessionProviderTests, AnUnknownModelFailsTheTurnWithTheClisMessage)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-unknown-model.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    AgentTurnRequest request = Request("hello");
    request.Model = "not-a-model";
    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code reported an error: There's an issue with the selected model "
                                   "(not-a-model). It may not exist or you may not have access to it. Run "
                                   "--model to pick a different model.");
    EXPECT_FALSE(events.Result.SessionNotFound);
}

// `codex exec resume <unknown id>` prints no record; its stderr says the thread is unknown.
TEST(CliSessionProviderTests, AnUnknownCodexThreadFailsTheTurnAsSessionNotFound)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "unknown-thread");
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;
    AgentTurnRequest request = Request("hello");
    request.SessionId = "00000000-0000-4000-8000-000000000009";

    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("no rollout found for thread id"), std::string::npos) << events.Result.Error;
    EXPECT_TRUE(events.Result.SessionNotFound);
}

TEST(CliSessionProviderTests, ThePromptGoesOnStdin)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "echo");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("a prompt with spaces"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.Text, "stdin=a prompt with spaces");
}

TEST(CliSessionProviderTests, TheChildEnvironmentHoldsNoKeyVariableButKeepsTheRest)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "env");
    ScopedEnvironmentVariable anthropicKey("ANTHROPIC_API_KEY", "fixture-anthropic-key");
    ScopedEnvironmentVariable anthropicToken("ANTHROPIC_AUTH_TOKEN", "fixture-anthropic-token");
    ScopedEnvironmentVariable codexKey("CODEX_API_KEY", "fixture-codex-key");
    ScopedEnvironmentVariable openAiKey("OPENAI_API_KEY", "fixture-openai-key");
    ScopedEnvironmentVariable unrelated("FAKE_AGENT_CLI_UNRELATED", "kept");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Result.Text, "ANTHROPIC_API_KEY unset; ANTHROPIC_AUTH_TOKEN unset; CODEX_API_KEY unset; "
                                  "OPENAI_API_KEY unset; FAKE_AGENT_CLI_UNRELATED=kept");
}

TEST(CliSessionProviderTests, CancellingMidStreamStopsTheTurnAndKeepsTheText)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "stall");
    ClaudeSessionProvider provider(FakeCli(), {});
    CancelToken cancel;

    // Cancels as soon as the first delta arrives; the fake then sleeps 30 s.
    class CancellingEvents final : public RecordingTurnEvents
    {
    public:
        explicit CancellingEvents(CancelToken& token) : m_Token(token) {}
        void OnTextDelta(std::string_view text) override
        {
            RecordingTurnEvents::OnTextDelta(text);
            m_Token.Cancel();
        }

    private:
        CancelToken& m_Token;
    } events(cancel);

    const auto start = std::chrono::steady_clock::now();
    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_LT(std::chrono::steady_clock::now() - start, 20s);
    EXPECT_EQ(events.FinishedCount, 1);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Stopped);
    EXPECT_EQ(events.Result.Text, "first");
}

TEST(CliSessionProviderTests, StoppingDuringTheVersionCheckStopsTheTurn)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "slow-version");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    // The fake sleeps 60 s before answering `--version`.
    std::thread stopper([&cancel]
    {
        std::this_thread::sleep_for(200ms);
        cancel.Cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    provider.RunTurn(Request("hello"), events, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    EXPECT_LT(elapsed, 10s);
    EXPECT_EQ(events.FinishedCount, 1);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Stopped) << events.Result.Error;
}

TEST(CliSessionProviderTests, AVersionCheckThatDoesNotAnswerFailsTheTurnWithTheFix)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "slow-version");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    const auto start = std::chrono::steady_clock::now();
    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_LT(std::chrono::steady_clock::now() - start, 40s);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_TRUE(events.Result.NotReady);
    EXPECT_EQ(events.Result.Error, "The program at " + FakeCli() +
                                       " did not answer \"claude --version\" within 20 s. If this is Claude Code, "
                                       "run \"claude --version\" in a terminal to see what it waits for. " +
                                       kClaudePanelFix);
}

TEST(CliSessionProviderTests, ACliThatExitsBeforeItsResultFailsTheTurnWithItsMessageAndKeepsTheText)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "exit-early");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code exited with code 3 before finishing the reply: fixture failure");
    EXPECT_EQ(events.Result.Text, "partial");
}

TEST(CliSessionProviderTests, AnOverlongLineFailsTheTurnWithItsMessage)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "overlong");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "The reply exceeded 1 MiB in one line, so it could not be read whole.");
}

TEST(CliSessionProviderTests, AClaudeOlderThanTheFloorIsRefusedWithTheFix)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "version 2.1.200");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    const std::vector<std::string> expected = {"finished:"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_TRUE(events.Result.NotReady);
    EXPECT_EQ(events.Result.Error, "Claude Code 2.1.200 at " + FakeCli() +
                                       ". Claude Code 2.1.259 or later is required: run \"claude update\".");
}

// Issue #6287: a program that prints nothing and exits 0 (the desktop app picked by
// mistake) is named as not Claude Code, with what it printed and the fix.
TEST(CliSessionProviderTests, AProgramThatPrintsNothingIsRefusedAsNotClaudeCode)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "silent");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_TRUE(events.Result.NotReady);
    const std::string verdict = "This program is not Claude Code: " + FakeCli() + " answered nothing when asked for its version.";
    EXPECT_EQ(events.Result.Error.rfind(verdict, 0), 0u) << events.Result.Error;
    EXPECT_NE(events.Result.Error.find(kClaudePanelFix), std::string::npos) << events.Result.Error;
    // The panel has no Browse button.
    EXPECT_EQ(events.Result.Error.find("Browse"), std::string::npos) << events.Result.Error;
}

TEST(CliSessionProviderTests, AMissingExecutableIsRefusedWithTheFix)
{
    ClaudeSessionProvider provider((TestDirectory() / "NoSuchAgentCli.exe").string(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("claude.com/claude-code"), std::string::npos) << events.Result.Error;
    EXPECT_EQ(events.Result.Error.find("set its path"), std::string::npos) << events.Result.Error;
}

// Only Windows runs .cmd and .bat files through cmd.exe; elsewhere the name is an ordinary file's.
TEST(CliSessionProviderTests, ACommandScriptIsRefusedWithTheFixOnlyOnWindows)
{
    const std::string script = (TestDirectory() / "claude.CMD").string();
    ClaudeSessionProvider provider(script, {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
#if defined(_WIN32)
    EXPECT_EQ(events.Result.Error, script + " is a command script, which the editor does not run. " + kClaudePanelFix);
#else
    EXPECT_EQ(events.Result.Error.find("command script"), std::string::npos) << events.Result.Error;
#endif
}

TEST(CliSessionProviderTests, TheCodexFixNamesTheProgramToPickAndWhereToGetIt)
{
    const std::string missing = (TestDirectory() / "NoSuchAgentCli.exe").string();
    const CancelToken cancel;

    const SetupStatus page = CodexSessionProvider(missing, {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
    const SetupStatus panel = CodexSessionProvider(missing, {}).CheckSetup(cancel, SetupSurface::Panel, {});

    ASSERT_FALSE(page.Steps.empty());
    ASSERT_FALSE(panel.Steps.empty());
    EXPECT_EQ(page.Steps[0].Fix,
              "Click Browse to pick " + kCodexToPick + ", or install the Codex CLI from github.com/openai/codex.");
    EXPECT_EQ(panel.Steps[0].Fix, "The Codex executable field is in Settings > AI Assistant: pick " + kCodexToPick +
                                      " there, or install the Codex CLI from github.com/openai/codex.");
}

TEST(CliSessionProviderTests, LoginStepsShowTheMethodAndNeverTheAccountOrAKey)
{
    const CancelToken cancel;
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "");
        const SetupStatus claude = ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        ASSERT_EQ(claude.Steps.size(), 2u);
        EXPECT_EQ(claude.Steps[1].State, SetupStepState::Passed);
        EXPECT_EQ(claude.Steps[1].Found, "Logged in (claude.ai).");
        EXPECT_TRUE(claude.Steps[1].FirstOutput.empty());

        const SetupStatus codex = CodexSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        ASSERT_EQ(codex.Steps.size(), 2u);
        EXPECT_EQ(codex.Steps[1].State, SetupStepState::Passed);
        EXPECT_EQ(codex.Steps[1].Found, "Logged in using an API key");
        EXPECT_TRUE(codex.Steps[1].FirstOutput.empty());
    }
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "logged-out");
        const SetupStatus codex = CodexSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        ASSERT_EQ(codex.Steps.size(), 2u);
        EXPECT_EQ(codex.Steps[1].State, SetupStepState::Failed);
        EXPECT_EQ(codex.Steps[1].Found, "Not logged in.");
        EXPECT_EQ(codex.Steps[1].Fix, "Run \"codex login\" in a terminal, then click Check again.");
    }
}

TEST(CliSessionProviderTests, TheLoginCheckRunsWithoutTheKeySoItReportsTheLogin)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "");
    ScopedEnvironmentVariable key("ANTHROPIC_API_KEY", "fixture-anthropic-key");
    const CancelToken cancel;

    const SetupStatus claude = ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});

    ASSERT_EQ(claude.Steps.size(), 2u);
    EXPECT_EQ(claude.Steps[1].State, SetupStepState::Passed);
    EXPECT_EQ(claude.Steps[1].Found, "Logged in (claude.ai).");
}

TEST(CliSessionProviderTests, CheckSetupFailsAProgramThatPrintsNothingAndLeavesTheLoginWaiting)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "silent");
    const CancelToken cancel;

    const SetupStatus status = ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 2u);
    const SetupStep& program = status.Steps[0];
    EXPECT_EQ(program.Name, "Claude Code");
    EXPECT_EQ(program.State, SetupStepState::Failed);
    EXPECT_EQ(program.Program, FakeCli());
    EXPECT_TRUE(program.FirstOutput.empty());
    EXPECT_EQ(program.Found,
              "This program is not Claude Code: " + FakeCli() + " answered nothing when asked for its version.");
    EXPECT_EQ(program.ExitCode, 0);
    EXPECT_EQ(program.Fix.rfind("Click Browse", 0), 0u) << program.Fix;
    EXPECT_NE(program.Fix.find("The Claude desktop app is a different program"), std::string::npos) << program.Fix;
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Waiting);
    EXPECT_FALSE(status.Ready());
}

// Support reads a failed check from the editor log: one line per step, with the program,
// its exit code and what it printed, and never the account a login step read.
TEST(CliSessionProviderTests, CheckSetupLogsEachStepWithoutTheAccountOrAKey)
{
    const CancelToken cancel;
    std::vector<std::string> lines;
    {
        const TestLog::ScopedLogCapture capture(&lines);
        Logger::Log::Info("capture is live");
        {
            ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "silent");
            ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        }
        {
            ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "");
            ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
            CodexSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        }
    }

    ASSERT_EQ(TestLog::CountLinesContaining(lines, "capture is live"), 1u);
    EXPECT_EQ(TestLog::FirstLineContaining(lines, "AI Assistant setup: Claude Code: failed"),
              "AI Assistant setup: Claude Code: failed (program '" + FakeCli() +
                  "', exit code 0, first output ''). This program is not Claude Code: " + FakeCli() +
                  " answered nothing when asked for its version.");
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "AI Assistant setup: Logged in: waiting"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "exit code -1"), 0u);
    EXPECT_EQ(TestLog::FirstLineContaining(lines, "AI Assistant setup: Logged in: passed"),
              "AI Assistant setup: Logged in: passed (program '" + FakeCli() +
                  "', exit code 0, first output ''). Logged in (claude.ai).");
    for (const char* secret : {"user@example.com", "Example Org", "org-fixture", "sk-proj"})
        EXPECT_EQ(TestLog::CountLinesContaining(lines, secret), 0u) << secret;
}

TEST(CliSessionProviderTests, CheckSetupFailsAClaudeBelowTheMinimumAndPassesOneAtIt)
{
    const CancelToken cancel;
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "version 2.1.258");
        const SetupStatus status = ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        ASSERT_EQ(status.Steps.size(), 2u);
        EXPECT_EQ(status.Steps[0].State, SetupStepState::Failed);
        EXPECT_EQ(status.Steps[0].FirstOutput, "2.1.258 (Claude Code)");
        EXPECT_NE(status.Steps[0].Fix.find("run \"claude update\""), std::string::npos) << status.Steps[0].Fix;
        EXPECT_EQ(status.Steps[1].State, SetupStepState::Waiting);
    }
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "version 2.1.259");
        const SetupStatus status = ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});
        ASSERT_EQ(status.Steps.size(), 2u);
        EXPECT_EQ(status.Steps[0].State, SetupStepState::Passed);
        EXPECT_EQ(status.Steps[0].Found, "Claude Code 2.1.259 at " + FakeCli() + ".");
        EXPECT_TRUE(status.Steps[0].Fix.empty());
        EXPECT_TRUE(status.Ready());
    }
}

TEST(CliSessionProviderTests, CheckSetupFailsALoggedOutClaudeWithTheLoginCommand)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "logged-out");
    const CancelToken cancel;

    const SetupStatus status = ClaudeSessionProvider(FakeCli(), {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 2u);
    EXPECT_EQ(status.Steps[0].State, SetupStepState::Passed);
    EXPECT_EQ(status.Steps[1].Name, "Logged in");
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Failed);
    EXPECT_EQ(status.Steps[1].Found, "Not logged in.");
    EXPECT_EQ(status.Steps[1].Fix, "Run \"claude auth login\" in a terminal, then click Check again.");
    EXPECT_EQ(status.FirstProblem(), &status.Steps[1]);
}

TEST(CliSessionProviderTests, CheckSetupNamesTheInstallForAMissingExecutableAndNotThePageItIsOn)
{
    const std::string missing = (TestDirectory() / "NoSuchAgentCli.exe").string();
    const CancelToken cancel;

    const SetupStatus status = ClaudeSessionProvider(missing, {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 2u);
    const SetupStep& program = status.Steps[0];
    EXPECT_EQ(program.State, SetupStepState::Failed);
    EXPECT_EQ(program.Found, "Could not start Claude Code at " + missing + ".");
    EXPECT_EQ(program.Fix, "Click Browse to pick " + kClaudeToPick + ", or install it from claude.com/claude-code.");
    EXPECT_EQ(program.Found.find("Settings"), std::string::npos);
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Waiting);
}

// The settings page shows each step as it ends: the check hands every step to the sink in
// check order, a step that waits on a failed one included, and returns the same steps.
TEST(CliSessionProviderTests, CheckSetupHandsEachStepToTheSinkAsItEnds)
{
    const CancelToken cancel;
    for (const std::string& program : {FakeCli(), (TestDirectory() / "NoSuchAgentCli.exe").string()})
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "");
        std::vector<std::string> published;
        const SetupStatus status =
            ClaudeSessionProvider(program, {})
                .CheckSetup(cancel, SetupSurface::SettingsPage,
                            [&published](const SetupStep& step) { published.push_back(step.Name); });

        EXPECT_EQ(published, (std::vector<std::string>{"Claude Code", "Logged in"})) << program;
        ASSERT_EQ(status.Steps.size(), published.size()) << program;
    }
}

// Issue #6287: the reported program sat in the desktop app's Store folder. A program
// there gets the desktop-app hint once, whichever way its step failed.
TEST(CliSessionProviderTests, AMissingProgramUnderTheDesktopAppFolderGetsTheDesktopAppHint)
{
    const std::string missing = (TestDirectory() / "WindowsApps" / "Claude_x" / "app" / "claude.exe").string();
    const CancelToken cancel;

    const SetupStatus status = ClaudeSessionProvider(missing, {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 2u);
    const SetupStep& program = status.Steps[0];
    EXPECT_EQ(program.State, SetupStepState::Failed);
    EXPECT_EQ(program.Found, "Could not start Claude Code at " + missing + ".");
    EXPECT_EQ(program.Fix, "Click Browse to pick " + kClaudeToPick +
                               ", or install it from claude.com/claude-code. The Claude desktop app is a different "
                               "program: it cannot run the editor's AI Assistant.");
}

#if defined(_WIN32)
TEST(CliSessionProviderTests, CheckSetupRefusesACommandScriptWithoutRunningIt)
{
    const std::string script = (TestDirectory() / "claude.cmd").string();
    const CancelToken cancel;

    const SetupStatus status = ClaudeSessionProvider(script, {}).CheckSetup(cancel, SetupSurface::SettingsPage, {});

    ASSERT_EQ(status.Steps.size(), 2u);
    const SetupStep& program = status.Steps[0];
    EXPECT_EQ(program.State, SetupStepState::Failed);
    EXPECT_EQ(program.Found, script + " is a command script, which the editor does not run.");
    EXPECT_TRUE(program.FirstOutput.empty());
    EXPECT_EQ(status.Steps[1].State, SetupStepState::Waiting);
}
#endif

TEST(CliSessionProviderTests, ALoggedOutSessionRefusesTheTurnWithTheFix)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "logged-out");
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_TRUE(events.Result.NotReady);
    EXPECT_EQ(events.Result.Error, "Not logged in. Run \"codex login\" in a terminal, then send the prompt again.");
}

// A reply whose text comes in two blocks around a tool call (issue #3435): the second block
// starts a new paragraph, as the model wrote them, instead of running on from the first's
// last word. The break travels with the second block's first delta.
TEST(CliSessionProviderTests, TextBlocksAroundAToolCallAreSeparatedByAParagraphBreak)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-two-text-blocks.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("Make a lantern and save."), events, cancel);

    const std::string reply = "I'll create the lantern, then save the scene.\n\nThe create_entity call succeeded.";
    const std::vector<std::string> expected = {std::string("session:") + kFixtureSession,
                                               "delta:I'll create the lantern,",
                                               "delta: then save the scene.",
                                               "tool:mcp__gameengine__create_entity",
                                               "delta:\n\nThe create_entity call succeeded.",
                                               "finished:" + reply};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Text, reply);
}

// Codex reports each message whole; a later message (after a command it ran) starts a new
// paragraph, as with Claude's text blocks.
TEST(CliSessionProviderTests, CodexMessagesAroundACommandAreSeparatedByAParagraphBreak)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("codex-two-messages.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("What is in the scene?"), events, cancel);

    const std::string reply = "I will look at the scene first.\n\nIt holds seven entities.";
    const std::vector<std::string> expected = {std::string("session:") + kFixtureSession,
                                               "delta:I will look at the scene first.",
                                               "tool:command_execution",
                                               "delta:\n\nIt holds seven entities.",
                                               "finished:" + reply};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Text, reply);
}
} // namespace GameEngine
