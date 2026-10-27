#include "Panels/SaveSceneAsModal.h"

#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Platform/Capabilities.h"
#include "Platform/Shell.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string_view>
#include <vector>

namespace GameEngine
{

namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/SaveSceneAsModal/SaveSceneAsModal.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/SaveSceneAsModal/SaveSceneAsModal.css";
constexpr const char* kFolderRowLayoutAssetPath = "UI/controls/SaveSceneAsModal/SaveSceneAsFolderRow.uxml";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 7> kRequiredIds = {
    "save-scene-as-name",
    "save-scene-as-browse",
    "save-scene-as-directory",
    "save-scene-as-folders",
    "save-scene-as-warning",
    "save-scene-as-cancel",
    "save-scene-as-save",
};

static std::string SanitizeFileStem(std::string s)
{
    // Very small sanitizer: keep alnum, '_' '-' and space; replace others with '_'.
    for (auto& ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_' || ch == '-' || ch == ' ')
            continue;
        ch = '_';
    }
    // trim spaces
    while (!s.empty() && s.front() == ' ')
        s.erase(s.begin());
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    if (s.empty())
        s = "NewScene";
    return s;
}
} // namespace

SaveSceneAsModal::SaveSceneAsModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_NameField = dynamic_cast<TextField*>(backdrop->FindById("save-scene-as-name"_sid));
    m_DirLabel = dynamic_cast<Label*>(backdrop->FindById("save-scene-as-directory"_sid));
    m_FolderList = backdrop->FindById("save-scene-as-folders"_sid);
    m_OverwriteWarningLabel = dynamic_cast<Label*>(backdrop->FindById("save-scene-as-warning"_sid));
    m_SaveButton = dynamic_cast<Button*>(backdrop->FindById("save-scene-as-save"_sid));
    if (m_NameField)
        m_NameField->SetOnCommit([this]() { OnSaveClicked(); });
    backdrop->FindById("save-scene-as-browse"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });
    backdrop->FindById("save-scene-as-cancel"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    backdrop->FindById("save-scene-as-save"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnSaveClicked(); });

    backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
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

void SaveSceneAsModal::Show(const std::filesystem::path& initialDirectory)
{
    m_Visible = true;
    ClearOverwritePrompt();
    m_SelectedDir = initialDirectory;
    m_ProjectRoot = initialDirectory; // Confine the web folder chooser to this subtree.
    if (m_FolderList)
        m_FolderList->RemoveAllChildren();
    if (m_DirLabel)
        m_DirLabel->SetText(m_SelectedDir.empty() ? "(none)" : m_SelectedDir.string());

    AddClass("visible");

    if (m_NameField)
    {
        // Defer focus until after the next UI tick: Show() flips display from None to Block,
        // but the element isn't fully mounted/laid out until the dispatcher drains, so an
        // immediate FocusElement can no-op on a freshly visible subtree.
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

void SaveSceneAsModal::Hide()
{
    m_Visible = false;
    ClearOverwritePrompt();
    if (m_FolderList)
        m_FolderList->RemoveAllChildren();
    RemoveClass("visible");
}

void SaveSceneAsModal::OnBrowseClicked()
{
    if (Platform::ProjectStorageIsSandboxed())
    {
        // An OS folder dialog cannot name a location inside the project here (it
        // would import an arbitrary folder instead). Toggle the in-project
        // chooser: a second click closes it.
        if (m_FolderList && !m_FolderList->GetChildren().empty())
        {
            m_FolderList->RemoveAllChildren();
            return;
        }
        PopulateFolderList();
        return;
    }
    const std::filesystem::path picked = Platform::SelectFolder(m_SelectedDir);
    if (picked.empty())
        return;
    m_SelectedDir = picked;
    if (m_DirLabel)
        m_DirLabel->SetText(m_SelectedDir.string());
}

void SaveSceneAsModal::PopulateFolderList()
{
    if (!m_FolderList)
        return;
    m_FolderList->RemoveAllChildren();

    // Up one level, bounded at the project root so the destination cannot escape it.
    if (!m_ProjectRoot.empty() && m_SelectedDir != m_ProjectRoot && !AddFolderRow("[ .. ]", m_SelectedDir.parent_path()))
        return;

    // Immediate subfolders only: one level is the single read the UI thread may
    // make on every platform (see FileSystem::ListDirectories).
    std::vector<std::filesystem::path> folders;
    std::error_code listError;
    for (std::filesystem::directory_iterator it(m_SelectedDir, listError), end; !listError && it != end; it.increment(listError))
    {
        std::error_code entryError;
        if (!it->is_directory(entryError))
            continue;
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() != '.')
            folders.push_back(it->path());
    }
    std::sort(folders.begin(), folders.end());
    for (const std::filesystem::path& directory : folders)
        if (!AddFolderRow(directory.filename().string(), directory))
            return;
}

bool SaveSceneAsModal::AddFolderRow(const std::string& text, const std::filesystem::path& target)
{
    std::unique_ptr<UIElement> row = Editor::ControlLayoutLoader::Load(kFolderRowLayoutAssetPath, {});
    if (!row)
        return false;
    auto* button = dynamic_cast<Button*>(row.get());
    if (!button)
    {
        Logger::Log::Error("Save Scene As: {} needs a Button at its root, so the folder list cannot show.",
                           kFolderRowLayoutAssetPath);
        return false;
    }
    button->SetText(text);
    button->RegisterEventHandler(kEventButtonClick, [this, target](UIEvent&) {
        m_SelectedDir = target;
        if (m_DirLabel)
            m_DirLabel->SetText(m_SelectedDir.string());
        PopulateFolderList();
    });
    m_FolderList->AddChild(std::move(row));
    return true;
}

void SaveSceneAsModal::ClearOverwritePrompt()
{
    m_PendingOverwrite = false;
    m_PendingOverwritePath.clear();
    if (m_OverwriteWarningLabel)
        m_OverwriteWarningLabel->RemoveClass("visible");
    if (m_SaveButton)
        m_SaveButton->SetText("Save");
}

void SaveSceneAsModal::CommitSave(const std::filesystem::path& outPath)
{
    Hide();
    if (m_OnSave)
        m_OnSave(outPath);
}

void SaveSceneAsModal::OnSaveClicked()
{
    if (!m_NameField)
        return;
    std::string stem = SanitizeFileStem(m_NameField->GetValue());
    if (m_SelectedDir.empty())
        return;

    const std::filesystem::path outPath = (m_SelectedDir / (stem + ".scene")).lexically_normal();

    if (m_PendingOverwrite)
    {
        if (outPath != m_PendingOverwritePath)
            ClearOverwritePrompt();
        else
        {
            CommitSave(outPath);
            return;
        }
    }

    const bool fileExists =
        std::filesystem::exists(outPath) && std::filesystem::is_regular_file(outPath);
    if (fileExists)
    {
        m_PendingOverwrite = true;
        m_PendingOverwritePath = outPath;
        if (m_OverwriteWarningLabel)
        {
            m_OverwriteWarningLabel->SetText(
                "A scene file already exists at this path. Replace the existing file?");
            m_OverwriteWarningLabel->AddClass("visible");
        }
        if (m_SaveButton)
            m_SaveButton->SetText("Overwrite");
        return;
    }

    CommitSave(outPath);
}

void SaveSceneAsModal::OnCancelClicked()
{
    if (m_PendingOverwrite)
    {
        ClearOverwritePrompt();
        return;
    }
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
