#pragma once

#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine
{
class Button;
class Label;
} // namespace GameEngine

namespace GameEngine::EditorUI
{

/// A setup check's result: the summary head from SetupStepList.uxml (the state pill, the line
/// beside it, Check again), then the steps in check order, one SetupStepRow.uxml row each: the
/// state mark, the name, the finding and, for a failed or warning step, the fix in a notice.
class SetupStepList : public UIElement
{
public:
    /// Where one step is; each state is one mark class (setup-step-mark-<state>).
    enum class State : uint8_t
    {
        /// Not run yet: an earlier step failed, or the check has not reached it.
        Waiting,
        Running,
        Passed,
        /// Passed with a limit the user should know.
        Warning,
        Failed,
    };

    /// Where the whole check is; each state but NotChecked is one pill class
    /// (setup-status-pill-<state>).
    enum class Summary : uint8_t
    {
        NotChecked,
        Checking,
        Ready,
        /// Ready with a warning step.
        ReadyLimited,
        NotReady,
    };

    struct Step
    {
        std::string Name;
        State StepState = State::Waiting;
        /// What the check found; for a waiting step, what the step will check.
        std::string Found;
        /// The action that fixes a failed or warning step; empty for the other states.
        std::string Fix;
    };

    SetupStepList();

    /// Replaces the rows with one row per step of `steps`, so no row of an earlier check stays;
    /// no steps hide the rows' frame.
    void SetSteps(std::span<const Step> steps);
    /// Shows `summary` as the pill's color with the words `pill`, and `line` beside it.
    void SetSummary(Summary summary, std::string_view pill, std::string_view line);
    /// Called when the user clicks Check again.
    void SetOnCheckAgain(std::function<void()> onCheckAgain);
    /// The tooltip of Check again: what a check runs.
    void SetCheckAgainTooltip(std::string_view tooltip);

private:
    void AddRow(const Step& step);

    Label* m_Pill = nullptr;
    Label* m_Line = nullptr;
    Button* m_CheckAgain = nullptr;
    UIElement* m_Rows = nullptr;
    std::function<void()> m_OnCheckAgain;
};

} // namespace GameEngine::EditorUI
