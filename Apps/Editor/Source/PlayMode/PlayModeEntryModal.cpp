#include "PlayMode/PlayModeEntryModal.h"

#include "PlayMode/PlayModeEntryGate.h"

#include "Input/KeyCodes.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ProgressBar.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

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
constexpr std::array<std::string_view, 5> kRequiredIds = {
    "play-entry-title",
    "play-entry-message",
    "play-entry-activity",
    "play-entry-progress",
    "play-entry-button",
};
constexpr const char* kFailedClass = "play-entry-failed";
constexpr const char* kPulseDimClass = "play-entry-progress-dim";
// Half a period of the indeterminate pulse: the bar's opacity transition in the stylesheet runs
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
    m_Progress = dynamic_cast<ProgressBar*>(m_Backdrop->FindById("play-entry-progress"_sid));
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
    if (!m_Title || !m_Message || !m_Activity || !m_Button || !m_Progress)
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
        m_Progress->AddClass(kHiddenClass);
        m_Button->SetText("Close");
        m_Button->SetTooltip("");
        return;
    }
    m_Title->SetText("Entering Play Mode");
    m_Message->SetText("Scripts are building. Play mode starts when the build finishes.");
    m_Activity->SetText(snapshot.Activity);
    m_Activity->RemoveClass(kFailedClass);
    m_Progress->RemoveClass(kHiddenClass);
    // The bar is indeterminate, and pulses, only while the build gives no measure. From the first
    // measure, 0 included, it shows the value that the activity line prints as a percent.
    const bool indeterminate = snapshot.Fraction < 0.0f;
    m_Progress->SetIndeterminate(indeterminate);
    m_Progress->SetValueWithoutNotify(snapshot.Fraction);
    const auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
    const bool dim = indeterminate && (sinceEpoch / kPulseHalfPeriod) % 2 == 1;
    if (dim)
        m_Progress->AddClass(kPulseDimClass);
    else
        m_Progress->RemoveClass(kPulseDimClass);
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
    if (GetOwnerManager() && m_Backdrop)
    {
        UIElement* backdrop = m_Backdrop;
        backdrop->PostSafeAction([backdrop]()
        {
            if (UIManager* manager = backdrop->GetOwnerManager())
                manager->FocusElement(backdrop);
        });
    }
}

void PlayModeEntryModal::OnButtonClicked()
{
    m_Gate.Cancel();
    SetVisible(false);
}

} // namespace GameEngine
