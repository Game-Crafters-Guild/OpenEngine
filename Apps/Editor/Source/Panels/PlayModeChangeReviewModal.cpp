#include "Panels/PlayModeChangeReviewModal.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{

namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/PlayModeChangeReviewModal/PlayModeChangeReviewModal.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/PlayModeChangeReviewModal/PlayModeChangeReviewModal.css";
constexpr const char* kRowLayoutAssetPath = "UI/controls/PlayModeChangeReviewModal/PlayModeChangeReviewRow.uxml";
constexpr const char* kRowToggleClass = "pmcr-toggle";
constexpr const char* kRowLabelClass = "pmcr-row-label";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 3> kRequiredIds = {
    "pmcr-scroll",
    "pmcr-discard",
    "pmcr-apply",
};
} // namespace

PlayModeChangeReviewModal::PlayModeChangeReviewModal()
{
    AddClass("playmode-change-review-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_Backdrop->FindById("pmcr-discard"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnDiscardClicked(); });
    m_Backdrop->FindById("pmcr-apply"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnApplyClicked(); });

    // A scroll view takes its content through its viewport, which a layout file cannot name.
    auto* scroll = dynamic_cast<ScrollView*>(m_Backdrop->FindById("pmcr-scroll"_sid));
    if (scroll)
    {
        auto listRoot = std::make_unique<UIElement>();
        listRoot->AddClass("pmcr-list");
        m_ListRoot = listRoot.get();
        scroll->AddContent(std::move(listRoot));
    }

    AddChild(std::move(backdrop));
}

void PlayModeChangeReviewModal::Show(const std::vector<std::string>& changeNames)
{
    RebuildList(changeNames);
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void PlayModeChangeReviewModal::Hide()
{
    RemoveClass("visible");
}

void PlayModeChangeReviewModal::RebuildList(const std::vector<std::string>& changeNames)
{
    if (!m_ListRoot)
        return;

    // If we're in the middle of event dispatch, defer rebuilding until it's safe.
    if (UIElement::IsInEventDispatch())
    {
        std::vector<std::string> copy = changeNames;
        PostAction([this, copy]() { this->RebuildList(copy); });
        return;
    }

    // Clear existing rows (UIElement has no ClearChildren helper).
    m_ListRoot->RemoveAllChildren();
    m_Toggles.clear();
    m_Toggles.reserve(changeNames.size());

    for (const auto& name : changeNames)
    {
        std::unique_ptr<UIElement> row = Editor::ControlLayoutLoader::Load(kRowLayoutAssetPath, {});
        if (!row)
            return;
        auto* toggle = dynamic_cast<Toggle*>(InspectorDrag::FindChildByClass(row.get(), kRowToggleClass));
        auto* label = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(row.get(), kRowLabelClass));
        if (!toggle || !label)
        {
            Logger::Log::Error("Play Mode Changes: {} needs the '{}' toggle and the '{}' label.", kRowLayoutAssetPath,
                               kRowToggleClass, kRowLabelClass);
            return;
        }
        toggle->SetChecked(true);
        m_Toggles.push_back(toggle);
        label->SetText(name);
        m_ListRoot->AddChild(std::move(row));
    }
}

void PlayModeChangeReviewModal::OnApplyClicked()
{
    Hide();
    if (!m_OnApply)
        return;
    std::vector<bool> keep;
    keep.reserve(m_Toggles.size());
    for (auto* t : m_Toggles)
    {
        keep.push_back(t ? t->IsChecked() : false);
    }
    m_OnApply(keep);
}

void PlayModeChangeReviewModal::OnDiscardClicked()
{
    Hide();
    if (m_OnDiscard)
        m_OnDiscard();
}

} // namespace GameEngine
