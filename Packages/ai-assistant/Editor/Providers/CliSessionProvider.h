#pragma once

#include "CliSessionList.h"
#include "IAgentProvider.h"
#include "ReplyTextBlocks.h"

#include "Types/FlatMap.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace GameEngine
{
/// A local CLI session (Claude Code, Codex) that keeps its own transcript: each turn
/// spawns the user's own executable with the prompt on stdin and reads one JSON
/// record per stdout line. The child inherits the editor's environment minus every
/// key variable a CLI would prefer over the user's login (kStrippedVariables), so the
/// session runs on the login and never on a key. The derived class supplies the
/// command line and reads the records.
class CliSessionProvider : public IAgentProvider
{
public:
    /// The variables removed from every child's environment, version and login checks
    /// included.
    static constexpr std::string_view kStrippedVariables[] = {
        "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CODEX_API_KEY", "OPENAI_API_KEY"};

    /// `executable` is a program name searched for by the platform or a full path;
    /// `workingDirectory` is the session's directory (the project root), empty to
    /// inherit the editor's.
    CliSessionProvider(std::string executable, std::filesystem::path workingDirectory);

    ProviderCapabilities Capabilities() const override;

    /// The Editor tools step's name (ToolsStep).
    static constexpr std::string_view kEditorToolsStep = "Editor tools";

    /// Runs CheckSetup() until it passes once, then spawns the turn; a failed setup ends
    /// the turn Failed and NotReady with the first failed step's finding and the panel's fix. A turn that
    /// attaches the editor's tools runs without them when ToolsRefusal() refuses, and its
    /// TurnResult::Note says why. Ends Stopped when `cancel` fires, checks included; Failed when the
    /// CLI cannot start, a check does not answer within its time limit, a stdout line
    /// exceeds 1 MiB, the CLI reports an error or exits before its completion record;
    /// Text keeps what arrived in every case.
    void RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel) final;

    /// The program step (CheckProgram), then the login step (CheckLogin), both under the
    /// stripped environment. A completed check that passes lets the next turn skip it;
    /// one that fails makes the next turn check again.
    SetupStatus CheckSetup(const CancelToken& cancel, SetupSurface surface, const SetupStepSink& onStep) const final;
    /// The program step and the login step, as CheckSetup() runs them.
    std::vector<SetupStep> SetupPlan() const final;
    /// A CheckSetup() passed since the executable was set: a turn starts without a check.
    bool SetupChecked() const { return m_Ready.load(std::memory_order_acquire); }
    /// The Editor tools step for `node`: Passed when `refusal` is empty, else a Warning with
    /// `refusal` as its fix, since a turn then runs as a conversation only.
    static SetupStep ToolsStep(const std::string& node, std::string refusal);
    /// The Editor tools step before it runs, as SetupPlan() lists a step.
    static SetupStep PlannedToolsStep();

    /// The sessions the CLI keeps on disk for WorkingDirectory(), read from its own
    /// files (CliSessionFiles); empty without a working directory. Reads files for as
    /// long as that takes or until `cancel` fires: call it off the UI thread.
    virtual CliSessionList ListSessions(const CancelToken& cancel) const = 0;

    /// Empty when a turn can attach the editor's tools through the Node.js executable
    /// `node` to the debug port `port`; otherwise why not, with the fix. Checks that
    /// `node` is Node 20 or later, then the CLI's own condition (ToolsBlocker), and
    /// remembers the answer per node, port and surface (the fix names where Node
    /// executable is from `surface`). Blocks for as long as the checks take (seconds) or
    /// until `cancel` fires: call it off the UI thread.
    std::string ToolsRefusal(const std::string& node, uint16_t port, SetupSurface surface, const CancelToken& cancel);

    /// The executable this provider runs.
    const std::string& Executable() const { return m_Executable; }
    /// The directory sessions run in; empty inherits the editor's.
    const std::filesystem::path& WorkingDirectory() const { return m_WorkingDirectory; }

protected:
    /// One turn's progress, filled by ReadLine().
    struct TurnState
    {
        AgentTurnEvents& Events;
        TurnResult Result;
        /// Joins the reply's text blocks into Result.Text.
        ReplyTextBlocks TextBlocks;
        /// Set by ReadLine() on the record that ends the turn, with Result.Outcome
        /// Succeeded or Failed (Result.Error then says why).
        bool Completed = false;
    };

    /// What a short CLI command (a version or login check) printed.
    struct CommandOutput
    {
        /// Empty when the command ran to its end; otherwise why it did not (the
        /// executable is a .cmd or .bat script, could not start, was stopped or did
        /// not answer in time), with the fix.
        std::string Failure;
        /// The command did not answer within the check's time limit (Failure says so).
        bool TimedOut = false;
        /// The exit code; -1 when the executable could not be started.
        int ExitCode = -1;
        /// Every stdout line, each followed by '\n'.
        std::string Stdout;
        /// The end of stderr.
        std::string Stderr;
        /// The first non-empty line of Stdout, else of Stderr, cut at
        /// SetupStep::kMaxFirstOutputBytes.
        std::string FirstOutput;
    };

    /// The CLI's name in messages ("Claude Code").
    virtual std::string_view DisplayName() const = 0;
    /// The arguments of one turn; the prompt itself goes on stdin.
    virtual std::vector<std::string> TurnArguments(const AgentTurnRequest& request) const = 0;
    /// Reads one stdout line of the turn and reports through `turn.Events`.
    virtual void ReadLine(std::string_view line, TurnState& turn) const = 0;
    /// The program step: `<executable> --version` ran and printed the CLI's version.
    virtual SetupStep CheckProgram(const CancelToken& cancel, SetupSurface surface) const = 0;
    /// The login step: the CLI's own login check.
    virtual SetupStep CheckLogin(const CancelToken& cancel, SetupSurface surface) const = 0;
    /// The program to pick in the executable field, by its file name on this platform
    /// ("Claude Code's claude.exe" on Windows).
    virtual std::string ProgramToPick() const = 0;
    /// Where the program comes from ("install it from claude.com/claude-code").
    virtual std::string InstallSource() const = 0;
    /// A sentence that tells the CLI from a desktop app of the same name, or empty: for a
    /// program that printed nothing (`printedNothing`) or one that sits under a known
    /// desktop-app folder. A hint, never the finding.
    virtual std::string DesktopAppHint(bool printedNothing) const = 0;
    /// The end of stderr of a turn that exited before its completion record says the
    /// session the turn continued does not exist (TurnResult::SessionNotFound). A CLI
    /// that reports it in its stdout records sets it in ReadLine() instead.
    virtual bool IsUnknownSessionError(std::string_view stderrTail) const;

    /// Empty when the model can reach the editor's port `port` only through the
    /// attached server; otherwise why not. A CLI whose model can run commands checks
    /// whether they can open that port (`node` runs the probe).
    virtual std::string ToolsBlocker(const std::string& node, uint16_t port, const CancelToken& cancel) const;

    /// Runs `<executable> <arguments>` with the stripped environment in the working
    /// directory, without a stdin payload, and waits for it until it exits, `cancel`
    /// fires or the check's time limit passes.
    CommandOutput RunCommand(const std::vector<std::string>& arguments, const CancelToken& cancel) const;
    /// RunCommand() for another program than the CLI (`program` names it in messages).
    CommandOutput RunProgram(const std::string& executable, std::string_view program,
                             const std::vector<std::string>& arguments, const CancelToken& cancel) const;
    /// The last non-empty line of `text` (a command's stderr), cut to the length a
    /// message quotes.
    static std::string QuotedLastLine(std::string_view text);
    /// The refusal for an executable that could not be started.
    std::string NotStartedMessage() const;
    /// The sentence that fixes a wrong or missing executable on `surface`: on the settings
    /// page, Browse; in the panel, where the executable field is, first.
    std::string ProgramFix(SetupSurface surface) const;
    /// The fix of a failed login: run `command` in a terminal, then check again the way
    /// `surface` offers (Check again on the page, the next prompt in the panel).
    static std::string LoginFix(std::string_view command, SetupSurface surface);
    /// The program step for `output` of `<executable> <command>` when it did not run to
    /// its end (Failure set) or printed nothing; nullopt when it printed something.
    std::optional<SetupStep> FailedProgramStep(const CommandOutput& output, std::string_view command,
                                               SetupSurface surface) const;
    /// The program step for a program that printed `output.FirstOutput`, which is not
    /// the CLI's answer to its version command.
    SetupStep NotTheCliStep(const CommandOutput& output, SetupSurface surface) const;
    /// Where the CLI keeps its files: the directory the environment variable
    /// `variable` names (the CLI reads the same environment), else `folder` in the
    /// user's home directory.
    static std::filesystem::path DataDirectory(const char* variable, const char* folder);

private:
    std::string m_Executable;
    std::filesystem::path m_WorkingDirectory;
    std::mutex m_ReadinessMutex;
    /// Written by CheckSetup() on any thread, read by the UI thread (SetupChecked()).
    mutable std::atomic<bool> m_Ready = false;
    /// ToolsRefusal() answers by node, port and surface. They are valid while
    /// AiAssistantSettings::SetupGeneration() equals m_ToolsGeneration.
    std::mutex m_ToolsMutex;
    FlatMap<std::tuple<std::string, uint16_t, SetupSurface>, std::string> m_ToolsRefusals;
    uint64_t m_ToolsGeneration = 0;
};
} // namespace GameEngine
