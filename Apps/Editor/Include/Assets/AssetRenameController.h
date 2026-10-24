#pragma once

#include "UI/InlineRenameSession.h"

#include <filesystem>
#include <functional>

namespace GameEngine {

class AssetManager;
class Label;
class UIElement;
namespace Editor { class UndoRedoService; }

/// One inline rename of an asset or folder in the Assets browser, on the shared
/// InlineRenameSession: the field is filled with the editable name (a file's stem,
/// a folder's whole name), the name is validated as the user types, and Enter
/// executes an undoable rename. Escape, or committing an unchanged name, ends the
/// session without touching disk; losing focus commits an accepted name and drops a
/// refused one.
class AssetRenameController
{
public:
    /// Fired after every apply of the rename — undo and redo included — with the
    /// path the asset has after that apply.
    using OnRenamed = std::function<void(const std::filesystem::path& renamedTo)>;

    /// `title` is the label the field stands in for; its parent hosts the field.
    /// `fieldClass` is the session's modifier class for the view, or null.
    /// Keyboard focus returns to `focusOnEnd` (the view the item lives in) when the
    /// session ends with Enter or Escape. A session already editing `itemPath` is left as it is; any
    /// other session in progress is cancelled first.
    void Begin(Label* title,
               UIElement* focusOnEnd,
               const std::filesystem::path& itemPath,
               const char* fieldClass,
               Editor::UndoRedoService* undo,
               AssetManager* assets,
               OnRenamed onRenamed);

    /// Called when a view binds `row` to show `shownPath` (a pooled row or cell reused on a
    /// scroll or a refresh). The session ends without renaming when `row` holds the field and
    /// shows another entry; a row bound again to the entry being renamed keeps its field.
    void OnRowBound(const UIElement& row, const std::filesystem::path& shownPath);

private:
    InlineRenameSession m_Session;
    std::filesystem::path m_ItemPath;
};

} // namespace GameEngine
