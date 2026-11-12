#include "UI/Controls/SetupStepList.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"

#include <array>
#include <memory>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kStyleAssetPath = "UI/controls/SetupStepList/SetupStepList.css";
constexpr const char* kLayoutAssetPath = "UI/controls/SetupStepList/SetupStepList.uxml";
constexpr const char* kRowLayoutAssetPath = "UI/controls/SetupStepList/SetupStepRow.uxml";
constexpr std::string_view kPillId = "SetupStepPill";
constexpr std::string_view kLineId = "SetupStepSummary";
constexpr std::string_view kCheckAgainId = "SetupStepCheckAgain";
constexpr std::string_view kRowsId = "SetupStepRows";
constexpr const char* kHiddenClass = "hidden";
constexpr const char* kPillChecking = "setup-status-pill-checking";
constexpr const char* kPillReady = "setup-status-pill-ready";
constexpr const char* kPillWarning = "setup-status-pill-warning";
constexpr const char* kPillFailed = "setup-status-pill-failed";
constexpr const char* kPillClasses[] = {kPillChecking, kPillReady, kPillWarning, kPillFailed};
constexpr const char* kMarkClass = "setup-step-mark";
constexpr const char* kNameClass = "setup-step-name";
constexpr const char* kDetailClass = "setup-step-detail";
constexpr const char* kFoundClass = "setup-step-found";
constexpr const char* kDividedClass = "setup-step-row-divided";
constexpr const char* kFoundWaitingClass = "setup-step-found-waiting";

const char* MarkClass(SetupStepList::State state)
{
    switch (state)
    {
    case SetupStepList::State::Waiting:
        return "setup-step-mark-waiting";
    case SetupStepList::State::Running:
        return "setup-step-mark-running";
    case SetupStepList::State::Passed:
        return "setup-step-mark-passed";
    case SetupStepList::State::Warning:
        return "setup-step-mark-warning";
    case SetupStepList::State::Failed:
        return "setup-step-mark-failed";
    }
    return "setup-step-mark-waiting";
}

// The pill's state class; NotChecked keeps the base class alone.
const char* PillClass(SetupStepList::Summary summary)
{
    switch (summary)
    {
    case SetupStepList::Summary::NotChecked:
        return nullptr;
    case SetupStepList::Summary::Checking:
        return kPillChecking;
    case SetupStepList::Summary::Ready:
        return kPillReady;
    case SetupStepList::Summary::ReadyLimited:
        return kPillWarning;
    case SetupStepList::Summary::NotReady:
        return kPillFailed;
    }
    return nullptr;
}

} // namespace

SetupStepList::SetupStepList()
{
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    constexpr std::array<std::string_view, 4> requiredIds{kPillId, kLineId, kCheckAgainId, kRowsId};
    std::unique_ptr<UIElement> layout = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, requiredIds);
    if (!layout)
        return;
    m_Pill = layout->FindById<Label>(HashStringId(kPillId)).Get();
    m_Line = layout->FindById<Label>(HashStringId(kLineId)).Get();
    m_CheckAgain = layout->FindById<Button>(HashStringId(kCheckAgainId)).Get();
    m_Rows = layout->FindById(HashStringId(kRowsId));
    if (!m_Pill || !m_Line || !m_CheckAgain)
    {
        Logger::Log::Error("SetupStepList: {} needs Labels '{}' and '{}' and a Button '{}'.", kLayoutAssetPath,
                           kPillId, kLineId, kCheckAgainId);
        m_Pill = nullptr;
        m_Line = nullptr;
        m_CheckAgain = nullptr;
        m_Rows = nullptr;
        return;
    }
    m_CheckAgain->SetOnClick([this](UIEvent&) {
        if (m_OnCheckAgain)
            m_OnCheckAgain();
    });
    AddChild(std::move(layout));
}

void SetupStepList::SetSteps(std::span<const Step> steps)
{
    if (!m_Rows)
        return;
    m_Rows->RemoveAllChildren();
    for (const Step& step : steps)
        AddRow(step);
    if (steps.empty())
        m_Rows->AddClass(kHiddenClass);
    else
        m_Rows->RemoveClass(kHiddenClass);
}

void SetupStepList::SetSummary(Summary summary, std::string_view pill, std::string_view line)
{
    if (!m_Pill)
        return;
    for (const char* pillClass : kPillClasses)
        m_Pill->RemoveClass(pillClass);
    if (const char* pillClass = PillClass(summary))
        m_Pill->AddClass(pillClass);
    m_Pill->SetText(std::string(pill));
    m_Line->SetText(std::string(line));
}

void SetupStepList::SetOnCheckAgain(std::function<void()> onCheckAgain)
{
    m_OnCheckAgain = std::move(onCheckAgain);
}

void SetupStepList::SetCheckAgainTooltip(std::string_view tooltip)
{
    if (m_CheckAgain)
        m_CheckAgain->SetTooltip(std::string(tooltip));
}

void SetupStepList::AddRow(const Step& step)
{
    std::unique_ptr<UIElement> row = Editor::ControlLayoutLoader::Load(kRowLayoutAssetPath, {});
    if (!row)
        return;
    UIElement* mark = InspectorDrag::FindChildByClass(row.get(), kMarkClass);
    auto* name = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(row.get(), kNameClass));
    UIElement* detail = InspectorDrag::FindChildByClass(row.get(), kDetailClass);
    auto* found = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(detail, kFoundClass));
    if (!mark || !name || !detail || !found)
    {
        Logger::Log::Error("SetupStepList: {} needs a '{}' element, a '{}' Label and a '{}' Label in '{}'.",
                           kRowLayoutAssetPath, kMarkClass, kNameClass, kFoundClass, kDetailClass);
        return;
    }
    if (!m_Rows->GetChildren().empty())
        row->AddClass(kDividedClass);
    mark->AddClass(MarkClass(step.StepState));
    name->SetText(step.Name);
    found->SetText(step.Found);
    if (step.StepState == State::Waiting)
        found->AddClass(kFoundWaitingClass);
    const bool problem = step.StepState == State::Failed || step.StepState == State::Warning;
    if (problem && !step.Fix.empty())
        detail->AddChild(std::make_unique<InspectorNotice>(
            step.Fix, step.StepState == State::Failed ? InspectorNotice::Kind::Error : InspectorNotice::Kind::Warning));
    m_Rows->AddChild(std::move(row));
}

} // namespace GameEngine::EditorUI
