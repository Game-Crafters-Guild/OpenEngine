#pragma once

#include "UI/UIElement.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class Button;
class Label;
class TextField;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// The Create Prefab dialog: the name, or the file names for several roots; the folder; the entity count;
/// the references that leave the prefab, one line each; and Create. A name in use turns Create into Replace,
/// or disables it with the reason while instances use the file; several roots take a number instead.
class CreatePrefabDialog final : public UIElement
{
  public:
    /// Creates one prefab per file of `prefabFiles`, in the order of the request's names. `replace` is
    /// true when the user chose Replace for the one file. False, with `outError` stating the cause, keeps
    /// the dialog open with the error.
    using CreateHandler = std::function<bool(const std::vector<std::filesystem::path>& prefabFiles, bool replace,
                                             std::string& outError)>;

    /// Why the existing file `prefabFile` cannot be replaced, with the fix; empty when it can.
    using ReplaceCheck = std::function<std::string(const std::filesystem::path& prefabFile)>;

    /// What the dialog opens with.
    struct Request
    {
        /// The name of each prefab, one per selected root.
        std::vector<std::string> Names;
        /// The number of entities the prefabs hold together.
        std::size_t EntityCount = 0;
        /// One line per entity field that names an entity outside the prefabs.
        std::vector<std::string> OutgoingReferences;
        std::filesystem::path Folder;
        /// The project's assets folder: a folder outside it is refused.
        std::filesystem::path AssetsRoot;
    };

    /// The element id of the dialog under the top element of the editor's UI, so that every open
    /// finds the one dialog again.
    static constexpr const char* kElementId = "create-prefab-dialog";

    CreatePrefabDialog();

    /// The reason `name` cannot name a prefab file, with the fix; empty when it can.
    static std::string DescribeNameProblem(const std::string& name);

    /// What happens to `count` entity fields that point at an entity outside the prefab; `count` > 0.
    static std::string DescribeOutgoingReferences(std::size_t count);

    /// The references note: what happens to the fields, then `lines`, one per field. Empty for no lines.
    static std::string ListOutgoingReferences(std::span<const std::string> lines, std::size_t prefabCount);

    /// Shows the dialog for `request`. `canReplace` answers for a name in use when there is one name.
    void Open(Request request, CreateHandler onCreate, ReplaceCheck canReplace);

    bool IsOpen() const { return m_Open; }

  private:
    void Close();
    void OnCreateClicked();
    void OnBrowseClicked();
    void OnNameEdited();
    void ShowFolder();
    // Names a file in the folder for each name, a number added to a name in use.
    void ShowFiles();
    void ShowError(const std::string& text);
    void ClearConflict();

    UIElement* m_Backdrop = nullptr;
    UIElement* m_NameRow = nullptr;
    UIElement* m_FilesRow = nullptr;
    Label* m_FilesLabel = nullptr;
    TextField* m_NameField = nullptr;
    Label* m_FolderLabel = nullptr;
    Label* m_CountLabel = nullptr;
    Label* m_ReferencesLabel = nullptr;
    Label* m_MessageLabel = nullptr;
    Button* m_BrowseButton = nullptr;
    Button* m_CreateButton = nullptr;
    std::filesystem::path m_Folder;
    std::filesystem::path m_AssetsRoot;
    // The names of several roots and the files they get; empty for one root, which the name field names.
    std::vector<std::string> m_Names;
    std::vector<std::filesystem::path> m_Files;
    // The file the user was told about: a second Create at this path replaces it.
    std::filesystem::path m_ConflictFile;
    CreateHandler m_OnCreate;
    ReplaceCheck m_CanReplace;
    bool m_Open = false;
};

} // namespace GameEngine::Editor
