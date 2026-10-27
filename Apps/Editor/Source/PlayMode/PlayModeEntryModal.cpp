#include "PlayMode/PlayModeEntryModal.h"

#include "PlayMode/PlayModeEntryGate.h"

#include "Input/KeyCodes.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/PlayModeEntryModal/PlayModeEntryModal.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/PlayModeEntryModal/PlayModeEntryModal.css";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 6> kRequiredIds = {
    "play-entry-title",
    "play-entry-message",
    "play-entry-activity",
    "play-entry-progress-track",
    "play-entry-progress-fill",
    "play-entry-button",
};
constexpr const char* kFailedClass = "play-entry-failed";
constexpr const char* kPulseDimClass = "play-entry-progress-dim";
// Half a period of the indeterminate pulse: the fill's opacity transition in the stylesheet runs
// over the same time.
constexpr std::chrono::milliseconds kPulseHalfPeriod{700};
constexpr const char* kHiddenClass = "hidden";

std::string JoinModules(const std::vector<std::string>& modules)
{
    std::string joined;
    for (const std::string& module : modules)
    {
        if (!joined.empty())
            joined += ", ";
        joined += module;
    }
    return joined;
}

} // namespace

PlayModeEntryModal::PlayModeEntryModal(Editor::PlayModeEntryGate& gate)
    : m_Gate(gate)
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_Title = dynamic_cast<Label*>(m_Backdrop->FindById("play-entry-title"_sid));
    m_Message = dynamic_cast<Label*>(m_Backdrop->FindById("play-entry-message"_sid));
    m_Activity = dynamic_cast<Label*>(m_Backdrop->FindById("play-entry-activity"_sid));
    m_ProgressTrack = m_Backdrop->FindById("play-entry-progress-track"_sid);
    m_ProgressFill = m_Backdrop->FindById("play-entry-progress-fill"_sid);
    m_Button = dynamic_cast<Button*>(m_Backdrop->FindById("play-entry-button"_sid));
    if (m_Button)
        m_Button->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnButtonClicked(); });
    m_Backdrop->RegisterEventHandler(kEventKeyDown,
                                     [this](UIEvent& e)
                                     {
                                         if (m_Visible && e.Key == Input::kKeyCode_Escape)
                                         {
                                             e.Handled = true;
                                             OnButtonClicked();
                                         }
                                     });
    AddChild(std::move(backdrop));
}

void PlayModeEntryModal::Poll()
{
    const bool shown = m_Gate.GetPhase() != Editor::PlayModeEntryGate::Phase::Idle;
    if (shown)
        Present();
    SetVisible(shown);
}

void PlayModeEntryModal::Present()
{
    if (!m_Title || !m_Message || !m_Activity || !m_Button || !m_ProgressTrack || !m_ProgressFill)
        return;
    const Editor::ScriptBuildSnapshot& snapshot = m_Gate.GetSnapshot();
    if (m_Gate.GetPhase() == Editor::PlayModeEntryGate::Phase::Failed)
    {
        m_Title->SetText("Script Build Failed");
        m_Message->SetText("Play mode did not start. Fix the error, then press Play again; the Log has the "
                           "full build output.");
        std::string failure = JoinModules(snapshot.FailedModules);
        if (!snapshot.FirstError.empty())
            failure += ": " + snapshot.FirstError;
        m_Activity->SetText(failure);
        m_Activity->AddClass(kFailedClass);
        m_ProgressTrack->AddClass(kHiddenClass);
        m_Button->SetText("Close");
        m_Button->SetTooltip("");
        return;
    }
    m_Title->SetText("Entering Play Mode");
    m_Message->SetText("Scripts are building. Play mode starts when the build finishes.");
    m_Activity->SetText(snapshot.Activity);
    m_Activity->RemoveClass(kFailedClass);
    m_ProgressTrack->RemoveClass(kHiddenClass);
    // Without a measure the full-width fill pulses, so a long compile does not read as stalled.
    const bool indeterminate = snapshot.Fraction <= 0.0f;
    const float fraction = indeterminate ? 1.0f : std::min(snapshot.Fraction, 1.0f);
    m_ProgressFill->Overrides().Set(Style::Width, StyleLength::Percent(fraction * 100.0f));
    const auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
    const bool dim = indeterminate && (sinceEpoch / kPulseHalfPeriod) % 2 == 1;
    if (dim)
        m_ProgressFill->AddClass(kPulseDimClass);
    else
        m_ProgressFill->RemoveClass(kPulseDimClass);
    m_ProgressFill->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    m_Button->SetText("Cancel");
    m_Button->SetTooltip("Stop waiting and stay in edit mode. The script build keeps running.");
}

void PlayModeEntryModal::SetVisible(bool visible)
{
    if (visible == m_Visible)
        return;
    m_Visible = visible;
    if (!visible)
    {
        RemoveClass("visible");
        return;
    }
    AddClass("visible");
    if (auto* manager = GetOwnerManager(); manager && m_Backdrop)
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void PlayModeEntryModal::OnButtonClicked()
{
    m_Gate.Cancel();
    SetVisible(false);
}

} // namespace GameEngine
