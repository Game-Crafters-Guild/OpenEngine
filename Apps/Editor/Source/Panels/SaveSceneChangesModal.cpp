#include "Panels/SaveSceneChangesModal.h"

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
constexpr const char* kLayoutAssetPath = "UI/controls/SaveSceneChangesModal/SaveSceneChangesModal.uxml";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 5> kRequiredIds = {
    "save-changes-title",
    "save-changes-message",
    "save-changes-cancel",
    "save-changes-dont-save",
    "save-changes-save",
};
} // namespace

SaveSceneChangesModal::SaveSceneChangesModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_Title = m_Backdrop->FindById("save-changes-title"_sid);
    m_Message = m_Backdrop->FindById("save-changes-message"_sid);
    m_CancelButton = m_Backdrop->FindById("save-changes-cancel"_sid);
    m_DontSaveButton = m_Backdrop->FindById("save-changes-dont-save"_sid);
    m_SaveButton = m_Backdrop->FindById("save-changes-save"_sid);
    if (m_CancelButton)
        m_CancelButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    if (m_DontSaveButton)
        m_DontSaveButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnDontSaveClicked(); });
    if (m_SaveButton)
        m_SaveButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnSaveClicked(); });

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnSaveClicked();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    AddChild(std::move(backdrop));
}

void SaveSceneChangesModal::Show(const std::string& title, const std::string& message,
                                 const SaveSceneChangesModalLabels& labels)
{
    if (!m_Backdrop)
        return;
    m_Visible = true;
    if (auto* t = dynamic_cast<Label*>(m_Title))
        t->SetText(title);
    if (auto* m = dynamic_cast<Label*>(m_Message))
        m->SetText(message);
    // Re-applied on every Show, so a prompt that customised them does not leak its wording into the
    // next caller's.
    if (auto* b = dynamic_cast<Button*>(m_SaveButton))
        b->SetText(labels.Save);
    if (auto* b = dynamic_cast<Button*>(m_DontSaveButton))
        b->SetText(labels.DontSave);
    if (auto* b = dynamic_cast<Button*>(m_CancelButton))
        b->SetText(labels.Cancel);
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void SaveSceneChangesModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void SaveSceneChangesModal::OnSaveClicked()
{
    Hide();
    if (m_OnSave)
        m_OnSave();
}

void SaveSceneChangesModal::OnDontSaveClicked()
{
    Hide();
    if (m_OnDontSave)
        m_OnDontSave();
}

void SaveSceneChangesModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
