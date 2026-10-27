#include "Panels/ConfirmActionModal.h"

#include "Input/KeyCodes.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{

namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/ConfirmActionModal/ConfirmActionModal.uxml";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 6> kRequiredIds = {
    "confirm-action-title",
    "confirm-action-message",
    "confirm-action-detail",
    "confirm-action-closing",
    "confirm-action-cancel",
    "confirm-action-confirm",
};
// The editor's generic hide class (theme/core.css `.hidden.hidden`, specificity
// 0,2,0, which outranks Button.css's `.button { display: flex }`). Takes the
// Cancel button out of layout while the modal is a notice, and a notice-only
// text block while it is empty or the modal is a confirmation.
constexpr const char* kHiddenClass = "hidden";
// Modal.css: keeps the caller's line breaks and does not wrap. Show's messages
// are laid out by their callers; a notice's message wraps instead.
constexpr const char* kPreformattedMessageClass = "modal-message-pre";
// Modal.css: lets a wrapped message break inside a word, so a file path with
// no spaces wraps at the window edge instead of running past it.
constexpr const char* kBreakableMessageClass = "modal-message-breakable";
// Modal.css: the secondary line of a notice (a location, a detail), in
// --ui_color_text_dim, which is darker than the message text.
constexpr const char* kDetailMessageClass = "modal-message-detail";

// Shows `text` in `block`, or takes the block out of layout when there is none.
void SetOptionalText(UIElement* block, const std::string& text)
{
    auto* label = dynamic_cast<Label*>(block);
    if (!label)
        return;
    label->SetText(text);
    if (text.empty())
        label->AddClass(kHiddenClass);
    else
        label->RemoveClass(kHiddenClass);
}
} // namespace

ConfirmActionModal::ConfirmActionModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_Title = m_Backdrop->FindById("confirm-action-title"_sid);
    m_Message = m_Backdrop->FindById("confirm-action-message"_sid);
    m_Detail = m_Backdrop->FindById("confirm-action-detail"_sid);
    m_Closing = m_Backdrop->FindById("confirm-action-closing"_sid);
    m_CancelBtn = dynamic_cast<Button*>(m_Backdrop->FindById("confirm-action-cancel"_sid));
    m_ConfirmBtn = dynamic_cast<Button*>(m_Backdrop->FindById("confirm-action-confirm"_sid));
    if (m_CancelBtn)
        m_CancelBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    if (m_ConfirmBtn)
        m_ConfirmBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnConfirmClicked(); });

    // Enter and Escape reach this handler from the backdrop (Show focuses it)
    // and bubble up to it from the dismiss button (ShowNotice focuses that,
    // which handles Enter itself as a click).
    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible) return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnConfirmClicked();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    AddChild(std::move(backdrop));
}

void ConfirmActionModal::Show(const std::string& title, const std::string& message,
                              const std::string& confirmLabel)
{
    if (m_CancelBtn)
        m_CancelBtn->RemoveClass(kHiddenClass);
    if (m_Message)
    {
        m_Message->RemoveClass(kBreakableMessageClass);
        m_Message->AddClass(kPreformattedMessageClass);
    }
    SetOptionalText(m_Detail, {});
    SetOptionalText(m_Closing, {});
    Present(title, message, confirmLabel, m_Backdrop);
}

void ConfirmActionModal::ShowNotice(const Notice& notice)
{
    if (m_CancelBtn)
        m_CancelBtn->AddClass(kHiddenClass);
    if (m_Message)
    {
        m_Message->RemoveClass(kPreformattedMessageClass);
        m_Message->AddClass(kBreakableMessageClass);
    }
    SetOptionalText(m_Detail, notice.Detail);
    SetOptionalText(m_Closing, notice.Closing);
    Present(notice.Title, notice.Message, notice.DismissLabel, m_ConfirmBtn);
}

void ConfirmActionModal::Present(const std::string& title, const std::string& message,
                                 const std::string& confirmLabel, UIElement* focusTarget)
{
    if (!m_Backdrop)
        return;
    m_Visible = true;
    if (auto* lbl = dynamic_cast<Label*>(m_Title))
        lbl->SetText(title);
    if (auto* lbl = dynamic_cast<Label*>(m_Message))
        lbl->SetText(message);
    if (m_ConfirmBtn)
        m_ConfirmBtn->SetText(confirmLabel);
    AddClass("visible");
    if (auto* manager = GetOwnerManager(); manager && focusTarget)
        manager->PostToUI([manager, focusTarget]() { manager->FocusElement(focusTarget); });
}

void ConfirmActionModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void ConfirmActionModal::OnConfirmClicked()
{
    Hide();
    if (m_OnConfirm)
        m_OnConfirm();
}

void ConfirmActionModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
