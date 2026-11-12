#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
/// Where one setup step is.
enum class SetupStepState : uint8_t
{
    /// Not run: an earlier step failed, or the check has not reached it.
    Waiting,
    Running,
    Passed,
    /// Passed with a limit the user should know (the editor's tools are off, or the API key
    /// could not be checked).
    Warning,
    Failed,
};

/// Where a setup check's fixes are shown: a fix names only the controls that place has.
enum class SetupSurface : uint8_t
{
    /// Settings > AI Assistant, with the executable fields and their Browse buttons.
    SettingsPage,
    /// The AI Assistant panel, which has no executable field.
    Panel,
};

/// One check a provider runs before a turn: the program, the login, the editor's tools.
struct SetupStep
{
    /// What the step checks, in the user's words ("Claude Code", "Logged in").
    std::string Name;
    SetupStepState State = SetupStepState::Waiting;
    /// What the check found ("Claude Code 2.1.300 at C:\...\claude.exe").
    std::string Found;
    /// Empty unless the step failed or warns: the action that fixes it.
    std::string Fix;
    /// The executable the step ran; empty when it ran none.
    std::string Program;
    /// The program's exit code; -1 when it ran none or could not start.
    int ExitCode = -1;
    /// The first non-empty line the program printed (stdout, else stderr), at most
    /// kMaxFirstOutputBytes.
    std::string FirstOutput;

    static constexpr std::size_t kMaxFirstOutputBytes = 200;
};

/// Gets each step of a setup check when the step ends, in check order, on the check's thread.
/// May be empty.
using SetupStepSink = std::function<void(const SetupStep&)>;

/// A provider's setup, step by step in check order (IAgentProvider::CheckSetup).
struct SetupStatus
{
    std::vector<SetupStep> Steps;

    /// Every step passed or warns: a turn can run.
    bool Ready() const
    {
        return std::all_of(Steps.begin(), Steps.end(), [](const SetupStep& step) {
            return step.State == SetupStepState::Passed || step.State == SetupStepState::Warning;
        });
    }

    /// The first failed step, else the first warning; null when every step passed.
    const SetupStep* FirstProblem() const
    {
        const SetupStep* warning = nullptr;
        for (const SetupStep& step : Steps)
        {
            if (step.State == SetupStepState::Failed)
                return &step;
            if (step.State == SetupStepState::Warning && !warning)
                warning = &step;
        }
        return warning;
    }
};
} // namespace GameEngine
