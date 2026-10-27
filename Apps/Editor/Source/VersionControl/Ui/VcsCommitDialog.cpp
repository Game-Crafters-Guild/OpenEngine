#include "VersionControl/Ui/VcsCommitDialog.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine::Editor {

namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/VcsCommitDialog/VcsCommitDialog.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/VcsDialog.css";
// The ids Show looks up and VcsDialog.css styles. A layout that lacks one does not load.
constexpr std::array<std::string_view, 4> kRequiredIds = {
    "vcs-commit-dialog",
    "commit-message-field",
    "commit-button",
    "cancel-button",
};
} // namespace

VcsCommitDialog::VcsCommitDialog() = default;
VcsCommitDialog::~VcsCommitDialog() = default;

void VcsCommitDialog::Show(GameEngine::UIManager* uiManager,
                           const std::string& defaultMessage,
                           std::function<void(const std::string&)> onCommit,
                           std::function<void()> onCancel)
{
    if (!uiManager)
    {
        Logger::Log::Error("VcsCommitDialog: UIManager is null");
        return;
    }

    Hide(); // re-Show replaces the dialog instead of stacking a second one

    // A full-screen backdrop that centers a bounded window (VcsDialog.css).
    std::unique_ptr<UIElement> dialogRoot = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!dialogRoot)
        return;

    m_UIManager = uiManager;
    m_OnCommit = std::move(onCommit);
    m_OnCancel = std::move(onCancel);
    m_IsVisible = true;

    m_DialogRoot = dialogRoot.get();
    m_DialogRoot->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    m_DialogRoot->SetFocusable(true);
    m_DialogRoot->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (e.Key != Input::kKeyCode_Escape)
            return;
        e.Handled = true;
        Cancel();
    });

    if (auto* textField = dynamic_cast<TextField*>(m_DialogRoot->FindById("commit-message-field"_sid)))
        textField->SetValue(defaultMessage);

    if (auto* commitButton = dynamic_cast<Button*>(m_DialogRoot->FindById("commit-button"_sid)))
        commitButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Commit(); });

    if (auto* cancelButton = dynamic_cast<Button*>(m_DialogRoot->FindById("cancel-button"_sid)))
        cancelButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Cancel(); });

    // Add to UI root (the root owns the element; m_DialogRoot stays as the
    // non-owning handle for Hide).
    if (auto* root = uiManager->GetRootElement())
    {
        root->AddChild(std::move(dialogRoot));
        uiManager->PostToUI([this, uiManager]()
        {
            if (m_DialogRoot && m_UIManager == uiManager)
            {
                if (auto* field = dynamic_cast<GameEngine::TextField*>(
                        m_DialogRoot->FindById("commit-message-field"_sid)))
                {
                    uiManager->FocusElement(field);
                    field->SelectAll();
                }
            }
        });
    }
    else
    {
        m_DialogRoot = nullptr;
    }
}

void VcsCommitDialog::Commit()
{
    auto* field =
        m_DialogRoot ? dynamic_cast<TextField*>(m_DialogRoot->FindById("commit-message-field"_sid)) : nullptr;
    const std::string message = field ? field->GetValue() : std::string();
    if (!message.empty() && m_OnCommit)
        m_OnCommit(message);
    Hide();
}

void VcsCommitDialog::Cancel()
{
    if (m_OnCancel)
        m_OnCancel();
    Hide();
}

void VcsCommitDialog::Hide()
{
    m_IsVisible = false;
    if (m_DialogRoot && m_UIManager)
    {
        if (auto* root = m_UIManager->GetRootElement())
        {
            root->RemoveChild(m_DialogRoot);
        }
    }
    m_DialogRoot = nullptr;
    m_UIManager = nullptr;
}

} // namespace GameEngine::Editor
