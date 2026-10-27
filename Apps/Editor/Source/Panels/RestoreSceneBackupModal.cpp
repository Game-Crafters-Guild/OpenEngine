#include "Panels/RestoreSceneBackupModal.h"

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
constexpr const char* kLayoutAssetPath = "UI/controls/RestoreSceneBackupModal/RestoreSceneBackupModal.uxml";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 4> kRequiredIds = {
    "restore-backup-title",
    "restore-backup-message",
    "restore-backup-discard",
    "restore-backup-restore",
};
} // namespace

RestoreSceneBackupModal::RestoreSceneBackupModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_Title = m_Backdrop->FindById("restore-backup-title"_sid);
    m_Message = m_Backdrop->FindById("restore-backup-message"_sid);
    if (auto* discard = dynamic_cast<Button*>(m_Backdrop->FindById("restore-backup-discard"_sid)))
        discard->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnDiscardClicked(); });
    if (auto* restore = dynamic_cast<Button*>(m_Backdrop->FindById("restore-backup-restore"_sid)))
        restore->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnRestoreClicked(); });

    // Enter restores (the safe choice). Deliberately no Escape binding:
    // Discard deletes the autosaved work and must be an explicit click.
    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnRestoreClicked();
        }
    });

    AddChild(std::move(backdrop));
}

void RestoreSceneBackupModal::Show(const std::string& title, const std::string& message)
{
    if (!m_Backdrop)
        return;
    m_Visible = true;
    if (auto* t = dynamic_cast<Label*>(m_Title))
        t->SetText(title);
    if (auto* m = dynamic_cast<Label*>(m_Message))
        m->SetText(message);
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void RestoreSceneBackupModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void RestoreSceneBackupModal::OnRestoreClicked()
{
    Hide();
    if (m_OnRestore)
        m_OnRestore();
}

void RestoreSceneBackupModal::OnDiscardClicked()
{
    Hide();
    if (m_OnDiscard)
        m_OnDiscard();
}

} // namespace GameEngine
