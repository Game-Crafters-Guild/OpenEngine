#include "EditorContextMenu/DirectoryMenuEditItems.h"

#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorContextMenu/EditMenuGroup.h"
#include "UI/EditorIcons.h"

#include "Platform/ContextMenu.h"

namespace GameEngine
{

void AddDirectoryEditItems(ContextMenuBuilder& builder)
{
    builder.AddItem("---folder-edits", 0, MenuItemFlag_None, EditMenuGroup::kSeparatorPriority)
        .AddItem("Rename", kDirectoryCmdRename, MenuItemFlag_None, EditMenuGroup::kRenamePriority, EditorIcons::kPencil,
                 Editor::CatalogShortcutLabel("Assets", "Rename"))
        .AddItem("Delete", kDirectoryCmdDelete, MenuItemFlag_None, EditMenuGroup::kDeletePriority, EditorIcons::kTrash)
        .AddItem("---folder-edits-end", 0, MenuItemFlag_None, EditMenuGroup::kEndSeparatorPriority);
}

} // namespace GameEngine
