#include "Prefabs/CreatePrefabDialog.h"

#include "Assets/AssetCreation.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Platform/Capabilities.h"
#include "Platform/Shell.h"
#include "Types/StringUtils.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/CreatePrefabDialog/CreatePrefabDialog.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/CreatePrefabDialog/CreatePrefabDialog.css";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 11> kRequiredIds = {
    "create-prefab-name-row", "create-prefab-files-row", "create-prefab-files",      "create-prefab-name",
    "create-prefab-folder",   "create-prefab-count",     "create-prefab-references", "create-prefab-message",
    "create-prefab-browse",   "create-prefab-create",    "create-prefab-cancel",
};
constexpr const char* kVisibleClass = "visible";
constexpr const char* kHiddenClass = "hidden";
constexpr std::string_view kForbiddenNameCharacters = "<>:\"/\\|?*";
// Characters below this are control characters, which no file name may hold.
constexpr unsigned char kFirstPrintableCharacter = 0x20;

} // namespace

std::string CreatePrefabDialog::DescribeNameProblem(const std::string& name)
{
    if (name.empty())
        return "Enter a name for the prefab.";
    for (const char character : name)
    {
        const bool control = static_cast<unsigned char>(character) < kFirstPrintableCharacter;
        if (control || kForbiddenNameCharacters.find(character) != std::string_view::npos)
            return "A prefab name cannot contain < > : \" / \\ | ? or *. Change the name.";
    }
    if (name.back() == '.')
        return "A prefab name cannot end in a period. Change the name.";
    return {};
}

namespace
{

// True for `folder` itself and every path under it.
bool IsInsideFolder(const std::filesystem::path& path, const std::filesystem::path& folder)
{
    const std::filesystem::path relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

CreatePrefabDialog::CreatePrefabDialog()
{
    SetId(kElementId);
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_NameRow = m_Backdrop->FindById("create-prefab-name-row"_sid);
    m_FilesRow = m_Backdrop->FindById("create-prefab-files-row"_sid);
    m_FilesLabel = dynamic_cast<Label*>(m_Backdrop->FindById("create-prefab-files"_sid));
    m_NameField = dynamic_cast<TextField*>(m_Backdrop->FindById("create-prefab-name"_sid));
    m_FolderLabel = dynamic_cast<Label*>(m_Backdrop->FindById("create-prefab-folder"_sid));
    m_CountLabel = dynamic_cast<Label*>(m_Backdrop->FindById("create-prefab-count"_sid));
    m_ReferencesLabel = dynamic_cast<Label*>(m_Backdrop->FindById("create-prefab-references"_sid));
    m_MessageLabel = dynamic_cast<Label*>(m_Backdrop->FindById("create-prefab-message"_sid));
    m_BrowseButton = dynamic_cast<Button*>(m_Backdrop->FindById("create-prefab-browse"_sid));
    m_CreateButton = dynamic_cast<Button*>(m_Backdrop->FindById("create-prefab-create"_sid));
    auto* cancelButton = dynamic_cast<Button*>(m_Backdrop->FindById("create-prefab-cancel"_sid));
    if (!m_NameRow || !m_FilesRow || !m_FilesLabel || !m_NameField || !m_FolderLabel || !m_CountLabel ||
        !m_ReferencesLabel || !m_MessageLabel || !m_BrowseButton || !m_CreateButton || !cancelButton)
    {
        Logger::Log::Error("CreatePrefabDialog: {} gives an id the dialog needs to the wrong kind of element. "
                           "Restore the element types.",
                           kLayoutAssetPath);
        m_Backdrop = nullptr;
        return;
    }
    // Enter presses Create; Escape reverts an edit first, then closes the dialog.
    m_NameField->SetOnEnter([this]() { OnCreateClicked(); });
    m_NameField->SetOnValueChanging([this](const std::string&) { OnNameEdited(); });
    m_BrowseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });
    m_CreateButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCreateClicked(); });
    cancelButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { Close(); });
    // Where no folder picker can name a folder inside the project, the dialog saves in the folder given.
    if (!Platform::SupportsFolderPicker() || Platform::ProjectStorageIsSandboxed())
        m_BrowseButton->AddClass(kHiddenClass);
    m_Backdrop->RegisterEventHandler(kEventKeyDown,
                                     [this](UIEvent& e)
                                     {
                                         if (m_Open && e.Key == Input::kKeyCode_Escape)
                                         {
                                             e.Handled = true;
                                             Close();
                                         }
                                     });
    AddChild(std::move(backdrop));
}

void CreatePrefabDialog::Open(Request request, CreateHandler onCreate, ReplaceCheck canReplace)
{
    if (!m_Backdrop || request.Names.empty())
        return;
    const std::size_t prefabCount = request.Names.size();
    m_Folder = std::move(request.Folder);
    m_AssetsRoot = std::move(request.AssetsRoot);
    m_OnCreate = std::move(onCreate);
    m_CanReplace = std::move(canReplace);
    m_NameField->SetValueWithoutNotify(request.Names.front());
    m_Names = prefabCount == 1 ? std::vector<std::string>{} : std::move(request.Names);
    // One prefab shows the name field; several show the list of file names.
    if (m_Names.empty())
    {
        m_NameRow->RemoveClass(kHiddenClass);
        m_FilesRow->AddClass(kHiddenClass);
    }
    else
    {
        m_NameRow->AddClass(kHiddenClass);
        m_FilesRow->RemoveClass(kHiddenClass);
    }
    ShowFolder();
    ShowFiles();
    const std::size_t entities = request.EntityCount;
    m_CountLabel->SetText(
        prefabCount > 1 ? std::format("Each root becomes one prefab: {} prefabs, {} entities.", prefabCount, entities)
        : entities == 1 ? std::string("The prefab holds 1 entity.")
                        : std::format("The prefab holds {} entities.", entities));
    const std::string references = ListOutgoingReferences(request.OutgoingReferences, prefabCount);
    m_ReferencesLabel->SetText(references);
    if (references.empty())
        m_ReferencesLabel->RemoveClass(kVisibleClass);
    else
        m_ReferencesLabel->AddClass(kVisibleClass);
    ClearConflict();
    m_Open = true;
    AddClass(kVisibleClass);
    // The name field, or Create for several roots, is focused after the next UI tick: the subtree is laid
    // out only when it is shown. The action runs only while the element is in its window.
    UIElement* focus = m_Names.empty() ? static_cast<UIElement*>(m_NameField) : m_CreateButton;
    focus->PostSafeAction(
        [focus, field = m_Names.empty() ? m_NameField : nullptr]()
        {
            if (UIManager* manager = focus->GetOwnerManager())
                manager->FocusElement(focus);
            if (field)
                field->SelectAll();
        });
}

void CreatePrefabDialog::Close()
{
    m_Open = false;
    m_OnCreate = nullptr;
    m_CanReplace = nullptr;
    ClearConflict();
    RemoveClass(kVisibleClass);
}

void CreatePrefabDialog::OnCreateClicked()
{
    if (!m_Open || !m_OnCreate || !m_CreateButton->IsEnabled())
        return;
    if (!m_Names.empty())
    {
        std::string error = std::format("Every name for a prefab in {} is taken. Choose another folder.",
                                        m_Folder.filename().string());
        const bool named = std::find(m_Files.begin(), m_Files.end(), std::filesystem::path()) == m_Files.end();
        if (!named || !m_OnCreate(m_Files, false, error))
        {
            ShowError(error);
            return;
        }
        Close();
        return;
    }
    const std::string name = TrimWhitespace(m_NameField->GetValue());
    if (const std::string problem = DescribeNameProblem(name); !problem.empty())
    {
        ShowError(problem);
        return;
    }
    const std::filesystem::path file = (m_Folder / (name + ".prefab")).lexically_normal();
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    const bool replace = exists && file == m_ConflictFile;
    if (exists && !replace)
    {
        if (std::string refusal = m_CanReplace ? m_CanReplace(file) : std::string(); !refusal.empty())
        {
            ShowError(refusal);
            m_CreateButton->SetEnabled(false);
            return;
        }
        m_ConflictFile = file;
        ShowError(std::format("A file named {} is already in this folder. Press Replace to overwrite it, or change "
                              "the name.",
                              file.filename().string()));
        m_CreateButton->SetText("Replace");
        return;
    }
    std::string error;
    if (!m_OnCreate({file}, replace, error))
    {
        ShowError(error);
        return;
    }
    Close();
}

void CreatePrefabDialog::OnBrowseClicked()
{
    const std::filesystem::path picked = Platform::SelectFolder(m_Folder);
    if (picked.empty())
        return;
    if (!IsInsideFolder(picked, m_AssetsRoot))
    {
        ShowError(std::format("{} is outside the project's assets. Choose a folder inside {}.", picked.string(),
                              m_AssetsRoot.string()));
        return;
    }
    m_Folder = picked;
    ShowFolder();
    ShowFiles();
    ClearConflict();
}

void CreatePrefabDialog::OnNameEdited()
{
    if (m_MessageLabel->HasClass(kVisibleClass))
        ClearConflict();
}

void CreatePrefabDialog::ShowFolder()
{
    const std::filesystem::path relative = m_Folder.lexically_normal().lexically_relative(m_AssetsRoot.lexically_normal());
    const std::string shown = relative.empty() || relative == "." ? std::string("Assets")
                                                                  : "Assets/" + relative.generic_string();
    m_FolderLabel->SetText(shown);
    m_FolderLabel->SetTooltip(m_Folder.string());
}

std::string CreatePrefabDialog::DescribeOutgoingReferences(std::size_t count)
{
    return count == 1 ? "1 field points at an entity outside the prefab. This instance keeps the reference; the "
                        "prefab file leaves the field empty."
                      : std::format("{} fields point at entities outside the prefab. This instance keeps the "
                                    "references; the prefab file leaves the fields empty.",
                                    count);
}

std::string CreatePrefabDialog::ListOutgoingReferences(std::span<const std::string> lines, std::size_t prefabCount)
{
    if (lines.empty())
        return {};
    const bool one = lines.size() == 1;
    std::string text =
        prefabCount > 1 ? (one ? "This field points at an entity outside the prefabs. Its instance keeps it; the "
                                 "prefab file leaves it empty."
                               : "These fields point at entities outside the prefabs. Each instance keeps them; the "
                                 "prefab files leave them empty.")
        : one ? "This field points at an entity outside the prefab. This instance keeps it; the prefab file leaves it "
                "empty."
              : "These fields point at entities outside the prefab. This instance keeps them; the prefab file leaves "
                "them empty.";
    for (const std::string& line : lines)
        text += "\n" + line;
    return text;
}

void CreatePrefabDialog::ShowFiles()
{
    m_Files.clear();
    std::string shown;
    for (const std::string& name : m_Names)
    {
        m_Files.push_back(MakeNumberedFilePath(m_Folder, name + ".prefab", m_Files));
        shown += (shown.empty() ? "" : ", ") + m_Files.back().filename().string();
    }
    m_FilesLabel->SetText(shown);
}

void CreatePrefabDialog::ShowError(const std::string& text)
{
    m_MessageLabel->SetText(text);
    m_MessageLabel->AddClass(kVisibleClass);
}

void CreatePrefabDialog::ClearConflict()
{
    m_ConflictFile.clear();
    m_MessageLabel->SetText("");
    m_MessageLabel->RemoveClass(kVisibleClass);
    m_CreateButton->SetText("Create");
    m_CreateButton->SetEnabled(true);
}

} // namespace GameEngine::Editor
