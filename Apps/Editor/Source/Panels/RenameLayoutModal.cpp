#include "Panels/RenameLayoutModal.h"

#include "Input/KeyCodes.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{

namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/RenameLayoutModal/RenameLayoutModal.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/RenameLayoutModal/RenameLayoutModal.css";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 4> kRequiredIds = {
    "rename-layout-title",
    "rename-layout-name",
    "rename-layout-cancel",
    "rename-layout-rename",
};
} // namespace

RenameLayoutModal::RenameLayoutModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Title = dynamic_cast<Label*>(m_Backdrop->FindById("rename-layout-title"_sid));
    m_NameField = dynamic_cast<TextField*>(m_Backdrop->FindById("rename-layout-name"_sid));
    if (m_NameField)
        m_NameField->SetOnCommit([this]() { OnCommitClicked(); });
    if (auto* cancel = dynamic_cast<Button*>(m_Backdrop->FindById("rename-layout-cancel"_sid)))
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    if (auto* rename = dynamic_cast<Button*>(m_Backdrop->FindById("rename-layout-rename"_sid)))
        rename->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCommitClicked(); });

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnCommitClicked();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    AddChild(std::move(backdrop));
}

void RenameLayoutModal::Show(const std::string& title, const std::string& initialValue)
{
    if (!m_Backdrop)
        return;
    m_Visible = true;
    if (m_Title)
        m_Title->SetText(title);
    if (m_NameField)
        m_NameField->SetValue(initialValue);

    AddClass("visible");

    if (m_NameField)
    {
        if (auto* mgr = GetOwnerManager())
        {
            TextField* field = m_NameField;
            mgr->PostToUI([mgr, field]()
            {
                mgr->FocusElement(field);
                field->SelectAll();
            });
        }
        else
        {
            m_NameField->SelectAll();
        }
    }
}

void RenameLayoutModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void RenameLayoutModal::OnCommitClicked()
{
    if (!m_NameField)
        return;
    const std::string value = m_NameField->GetValue();
    Hide();
    if (m_OnCommit)
        m_OnCommit(value);
}

void RenameLayoutModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
