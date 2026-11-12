#include "CliSessionProvider.h"

#include "AiAssistantSettings.h"
#include "CancelToken.h"

#include "Engine/Build/CancellableShellProcess.h"
#include "Logger/Logger.h"
#include "Platform/Process.h"
#include "Types/StringUtils.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
std::string_view StateName(SetupStepState state)
{
    switch (state)
    {
    case SetupStepState::Waiting:
        return "waiting";
    case SetupStepState::Running:
        return "running";
    case SetupStepState::Passed:
        return "passed";
    case SetupStepState::Warning:
        return "warning";
    case SetupStepState::Failed:
        return "failed";
    }
    return "unknown";
}

// One editor log line per step, so support can read a failed check after the fact.
// A login step never sets FirstOutput, so the account's details stay out of the log.
void LogStep(const SetupStep& step)
{
    // ExitCode -1 means that no exit code was read: the line does not show it.
    const std::string exitCode = step.ExitCode < 0 ? std::string() : ", exit code " + std::to_string(step.ExitCode);
    Logger::Log::Info("AI Assistant setup: {}: {} (program '{}'{}, first output '{}'). {}", step.Name,
                      StateName(step.State), step.Program, exitCode, step.FirstOutput, step.Found);
}

// A step that ends goes to the log and to the check's sink.
void EndStep(const SetupStep& step, const SetupStepSink& onStep)
{
    LogStep(step);
    if (onStep)
        onStep(step);
}

// A stderr line quoted in a failure message is cut here.
constexpr std::size_t kMaxQuotedStderrChars = 300;
// A version or login check that has not exited by then is ended and reported: the
// real CLIs answer in under 4 s.
constexpr std::chrono::seconds kCheckTimeout{20};

// The finding for `executable` that did not answer `command` within kCheckTimeout.
std::string NoAnswerText(const std::string& executable, std::string_view command)
{
    return "The program at " + executable + " did not answer \"" + std::string(command) + "\" within " +
           std::to_string(kCheckTimeout.count()) + " s";
}

// The verdict a finding leads with when the program answered but is not the CLI `cli`.
std::string NotTheCliVerdict(std::string_view cli)
{
    return "This program is not " + std::string(cli) + ": ";
}

// The oldest Node.js the MCP server supports (mcp/package.json "engines").
constexpr int kMinimumNodeMajor = 20;

constexpr std::string_view kLoggedInStep = "Logged in";

std::vector<Platform::EnvironmentEdit> StrippedEnvironment()
{
    std::vector<Platform::EnvironmentEdit> edits;
    for (std::string_view name : CliSessionProvider::kStrippedVariables)
        edits.push_back({std::string(name), std::nullopt});
    return edits;
}

// On Windows, a .cmd or .bat script runs through cmd.exe, which reparses every argument;
// npm's `claude.cmd` shim is one. Other platforms run no such scripts.
bool IsCommandScript(const std::string& executable)
{
#if defined(_WIN32)
    const std::string extension = std::filesystem::path(executable).extension().string();
    return EqualsIgnoreCase(extension, ".cmd") || EqualsIgnoreCase(extension, ".bat");
#else
    (void)executable;
    return false;
#endif
}

// The last non-empty line of `text`, cut at kMaxQuotedStderrChars.
std::string LastLine(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    const std::size_t start = text.find_last_of('\n');
    std::string_view line = start == std::string_view::npos ? text : text.substr(start + 1);
    if (line.size() > kMaxQuotedStderrChars)
        line = line.substr(0, kMaxQuotedStderrChars);
    return std::string(line);
}

// The first non-empty line of `text`, cut at SetupStep::kMaxFirstOutputBytes on a
// UTF-8 boundary.
std::string FirstNonEmptyLine(std::string_view text)
{
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = TrimWhitespaceView(text.substr(0, end));
        if (!line.empty())
        {
            if (line.size() > SetupStep::kMaxFirstOutputBytes)
            {
                std::size_t cut = SetupStep::kMaxFirstOutputBytes;
                while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
                    --cut;
                line = line.substr(0, cut);
            }
            return std::string(line);
        }
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return {};
}

#if defined(_WIN32)
constexpr const char* kHomeVariable = "USERPROFILE";
#else
constexpr const char* kHomeVariable = "HOME";
#endif

// The path the environment variable `name` holds; empty when it is unset. Read wide
// on Windows, so a profile path outside the ANSI code page survives.
std::filesystem::path EnvironmentPath(const char* name)
{
#if defined(_WIN32)
    const std::wstring wideName(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wideName.c_str());
#else
    const char* value = std::getenv(name);
#endif
    return value ? std::filesystem::path(value) : std::filesystem::path();
}
} // namespace

CliSessionProvider::CliSessionProvider(std::string executable, std::filesystem::path workingDirectory)
    : m_Executable(std::move(executable))
    , m_WorkingDirectory(std::move(workingDirectory))
{
}

ProviderCapabilities CliSessionProvider::Capabilities() const
{
    return {.Streams = true, .OwnsTranscript = true, .NeedsKey = false, .CanActWithTools = true};
}

std::string CliSessionProvider::ToolsRefusal(const std::string& node, uint16_t port, SetupSurface surface,
                                             const CancelToken& cancel)
{
    std::lock_guard lock(m_ToolsMutex);
    // Check again or a changed setting moves the generation: a Node.js installed since answers anew.
    if (const uint64_t generation = AiAssistantSettings::SetupGeneration(); generation != m_ToolsGeneration)
    {
        m_ToolsRefusals.Clear();
        m_ToolsGeneration = generation;
    }
    const auto key = std::make_tuple(node, port, surface);
    if (const std::string* known = m_ToolsRefusals.Find(key))
        return *known;

    const CommandOutput version = RunProgram(node, "Node.js", {"-p", "process.versions.node"}, cancel);
    std::string refusal;
    if (!version.Failure.empty() || version.ExitCode != 0)
    {
        const char* nodeRow = surface == SetupSurface::SettingsPage
                                  ? "set Node executable under Model and options"
                                  : "set its path in Settings > AI Assistant > Model and options > Node executable";
        const std::string failure =
            version.Failure.empty() ? "Node.js at " + node + " stopped with an error" : version.Failure;
        refusal = failure + ". Install Node.js " + std::to_string(kMinimumNodeMajor) + " or later, or " + nodeRow + ".";
    }
    else if (std::atoi(version.Stdout.c_str()) < kMinimumNodeMajor)
    {
        refusal = "Node.js " + LastLine(version.Stdout) + " is too old for the editor's tools. Install Node.js " +
                  std::to_string(kMinimumNodeMajor) + " or later.";
    }
    else
    {
        refusal = ToolsBlocker(node, port, cancel);
    }
    // A stopped check answers nothing: the next turn asks again.
    if (!cancel.IsCancelled())
        m_ToolsRefusals.InsertOrAssign(key, refusal);
    return refusal;
}

std::string CliSessionProvider::ToolsBlocker(const std::string&, uint16_t, const CancelToken&) const
{
    return {};
}

SetupStatus CliSessionProvider::CheckSetup(const CancelToken& cancel, SetupSurface surface,
                                           const SetupStepSink& onStep) const
{
    SetupStatus status;
    status.Steps.push_back(CheckProgram(cancel, surface));
    EndStep(status.Steps.back(), onStep);
    if (status.Steps.back().State == SetupStepState::Passed)
        status.Steps.push_back(CheckLogin(cancel, surface));
    else
        status.Steps.push_back({.Name = std::string(kLoggedInStep)});
    EndStep(status.Steps.back(), onStep);
    // A stopped check answers nothing: the next turn checks again.
    if (!cancel.IsCancelled())
        m_Ready.store(status.Ready(), std::memory_order_release);
    return status;
}

std::vector<SetupStep> CliSessionProvider::SetupPlan() const
{
    const std::string program(DisplayName());
    return {{.Name = program, .Found = "Finds " + program + " and reads its version."},
            {.Name = std::string(kLoggedInStep),
             .Found = "Checks your " + program + " login."}};
}

SetupStep CliSessionProvider::PlannedToolsStep()
{
    return {.Name = std::string(kEditorToolsStep),
            .Found = "Node.js " + std::to_string(kMinimumNodeMajor) +
                     " or later, so the assistant can act in the editor."};
}

SetupStep CliSessionProvider::ToolsStep(const std::string& node, std::string refusal)
{
    SetupStep step{.Name = std::string(kEditorToolsStep), .Program = node};
    if (refusal.empty())
    {
        step.State = SetupStepState::Passed;
        step.Found = "The assistant can use the editor's tools.";
    }
    else
    {
        step.State = SetupStepState::Warning;
        step.Found = "Conversation only: the assistant answers without the editor's tools.";
        step.Fix = std::move(refusal);
    }
    LogStep(step);
    return step;
}

void CliSessionProvider::RunTurn(const AgentTurnRequest& request, AgentTurnEvents& events, const CancelToken& cancel)
{
    TurnState turn{events};
    {
        std::lock_guard lock(m_ReadinessMutex);
        if (!m_Ready.load(std::memory_order_acquire))
        {
            const SetupStatus status = CheckSetup(cancel, SetupSurface::Panel, {});
            if (cancel.IsCancelled())
            {
                turn.Result.Outcome = TurnOutcome::Stopped;
                events.OnFinished(turn.Result);
                return;
            }
            if (const SetupStep* problem = status.FirstProblem(); problem && problem->State == SetupStepState::Failed)
            {
                turn.Result.NotReady = true;
                turn.Result.Error = problem->Found + " " + problem->Fix;
                events.OnFinished(turn.Result);
                return;
            }
        }
    }

    AgentTurnRequest attached = request;
    if (request.Tools)
    {
        const std::string refusal =
            ToolsRefusal(request.Tools->Node, request.Tools->Port, SetupSurface::Panel, cancel);
        if (cancel.IsCancelled())
        {
            turn.Result.Outcome = TurnOutcome::Stopped;
            events.OnFinished(turn.Result);
            return;
        }
        if (!refusal.empty())
        {
            attached.Tools = nullptr;
            turn.Result.Note = "This reply ran without the editor's tools. " + refusal;
        }
    }

    ProcessLaunch launch;
    launch.Executable = m_Executable;
    launch.Arguments = TurnArguments(attached);
    launch.WorkingDirectory = m_WorkingDirectory;
    launch.Environment = StrippedEnvironment();
    launch.StdinPayload = request.UserText;

    CancellableShellProcess process;
    const ShellProcessResult run = process.Run(
        launch, [&cancel] { return cancel.IsCancelled(); },
        [this, &turn](const std::string& line)
        {
            if (!turn.Completed)
                ReadLine(line, turn);
        });

    TurnResult& result = turn.Result;
    if (run.cancelled)
    {
        result.Outcome = TurnOutcome::Stopped;
    }
    else if (run.exitCode == -1)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = NotStartedMessage();
    }
    else if (run.overlongStdoutLines > 0)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = "The reply exceeded 1 MiB in one line, so it could not be read whole.";
    }
    else if (!turn.Completed)
    {
        result.Outcome = TurnOutcome::Failed;
        result.Error = std::string(DisplayName()) + " exited with code " + std::to_string(run.exitCode) +
                       " before finishing the reply";
        const std::string reason = LastLine(run.stderrTail);
        result.Error += reason.empty() ? "." : ": " + reason;
        result.SessionNotFound = !request.SessionId.empty() && IsUnknownSessionError(run.stderrTail);
    }
    events.OnFinished(result);
}

CliSessionProvider::CommandOutput CliSessionProvider::RunCommand(const std::vector<std::string>& arguments,
                                                                const CancelToken& cancel) const
{
    return RunProgram(m_Executable, DisplayName(), arguments, cancel);
}

CliSessionProvider::CommandOutput CliSessionProvider::RunProgram(const std::string& executable,
                                                                std::string_view program,
                                                                const std::vector<std::string>& arguments,
                                                                const CancelToken& cancel) const
{
    CommandOutput output;
    if (IsCommandScript(executable))
    {
        output.Failure = executable + " is a command script, which the editor does not run";
        return output;
    }

    ProcessLaunch launch;
    launch.Executable = executable;
    launch.Arguments = arguments;
    launch.WorkingDirectory = m_WorkingDirectory;
    launch.Environment = StrippedEnvironment();

    const auto deadline = std::chrono::steady_clock::now() + kCheckTimeout;
    CancellableShellProcess process;
    const ShellProcessResult run = process.Run(
        launch, [&cancel, deadline] { return cancel.IsCancelled() || std::chrono::steady_clock::now() >= deadline; },
        [&output](const std::string& line)
        {
            output.Stdout += line;
            output.Stdout += '\n';
        });
    output.ExitCode = run.exitCode;
    output.Stderr = run.stderrTail;
    output.FirstOutput = FirstNonEmptyLine(output.Stdout);
    if (output.FirstOutput.empty())
        output.FirstOutput = FirstNonEmptyLine(output.Stderr);
    if (run.cancelled && cancel.IsCancelled())
    {
        output.Failure = "Stopped";
    }
    else if (run.cancelled)
    {
        output.TimedOut = true;
        output.Failure = NoAnswerText(executable, std::filesystem::path(executable).stem().string() + " " +
                                                      Join(arguments, " "));
    }
    else if (run.exitCode == -1)
    {
        output.Failure = executable.empty() ? "No " + std::string(program) + " executable is set"
                                            : "Could not start " + std::string(program) + " at " + executable;
    }
    return output;
}

std::string CliSessionProvider::ProgramFix(SetupSurface surface) const
{
    if (surface == SetupSurface::Panel)
        return "The " + std::string(DisplayName()) + " executable field is in Settings > AI Assistant: pick " +
               ProgramToPick() + " there, or " + InstallSource() + ".";
    return "Click Browse to pick " + ProgramToPick() + ", or " + InstallSource() + ".";
}

std::string CliSessionProvider::LoginFix(std::string_view command, SetupSurface surface)
{
    const char* then = surface == SetupSurface::SettingsPage ? "click Check again" : "send the prompt again";
    return "Run \"" + std::string(command) + "\" in a terminal, then " + then + ".";
}

std::optional<SetupStep> CliSessionProvider::FailedProgramStep(const CommandOutput& output, std::string_view command,
                                                               SetupSurface surface) const
{
    SetupStep step{.Name = std::string(DisplayName()), .State = SetupStepState::Failed, .Program = m_Executable,
                   .ExitCode = output.ExitCode, .FirstOutput = output.FirstOutput};
    bool printedNothing = false;
    if (output.TimedOut)
    {
        // A real CLI can wait on a first-run prompt, an update or the network.
        step.Found = NoAnswerText(m_Executable, command) + ".";
        step.Fix = "If this is " + std::string(DisplayName()) + ", run \"" + std::string(command) +
                   "\" in a terminal to see what it waits for. " + ProgramFix(surface);
    }
    else if (!output.Failure.empty())
    {
        step.Found = output.Failure + ".";
        step.Fix = ProgramFix(surface);
    }
    else if (output.FirstOutput.empty())
    {
        printedNothing = true;
        step.Found = NotTheCliVerdict(DisplayName()) + m_Executable + " answered nothing when asked for its version.";
        step.Fix = ProgramFix(surface);
    }
    else
    {
        return std::nullopt;
    }
    if (const std::string hint = DesktopAppHint(printedNothing); !hint.empty())
        step.Fix += " " + hint;
    return step;
}

SetupStep CliSessionProvider::NotTheCliStep(const CommandOutput& output, SetupSurface surface) const
{
    SetupStep step{.Name = std::string(DisplayName()), .State = SetupStepState::Failed, .Program = m_Executable,
                   .ExitCode = output.ExitCode, .FirstOutput = output.FirstOutput};
    step.Found = NotTheCliVerdict(DisplayName()) + m_Executable + " answered \"" + output.FirstOutput +
                 "\" when asked for its version.";
    step.Fix = ProgramFix(surface);
    if (const std::string hint = DesktopAppHint(false); !hint.empty())
        step.Fix += " " + hint;
    return step;
}

std::string CliSessionProvider::QuotedLastLine(std::string_view text)
{
    return LastLine(text);
}

bool CliSessionProvider::IsUnknownSessionError(std::string_view) const
{
    return false;
}

std::filesystem::path CliSessionProvider::DataDirectory(const char* variable, const char* folder)
{
    if (std::filesystem::path named = EnvironmentPath(variable); !named.empty())
        return named;
    const std::filesystem::path home = EnvironmentPath(kHomeVariable);
    return home.empty() ? std::filesystem::path() : home / folder;
}

std::string CliSessionProvider::NotStartedMessage() const
{
    if (m_Executable.empty())
        return "No " + std::string(DisplayName()) + " executable: set its path in Settings > AI Assistant.";
    return "Could not start " + std::string(DisplayName()) + " (" + m_Executable +
           "): set its path in Settings > AI Assistant.";
}
} // namespace GameEngine
