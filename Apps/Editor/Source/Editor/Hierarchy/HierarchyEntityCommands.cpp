#include "Editor/Hierarchy/HierarchyEntityCommands.h"

#include "Editor/Shortcuts/EditorShortcuts.h"
#include "EditorContextMenu/EditMenuGroup.h"
#include "UI/EditorIcons.h"

#include "Platform/ContextMenu.h"

namespace GameEngine::Editor
{

void AddHierarchyEntityEditItems(ContextMenuBuilder& builder)
{
    builder.AddItem("---entity-edits", 0, MenuItemFlag_None, EditMenuGroup::kSeparatorPriority)
        .AddItem("Rename", kHierarchyCmdRename, MenuItemFlag_None, EditMenuGroup::kRenamePriority, EditorIcons::kPencil,
                 CatalogShortcutLabel("Hierarchy", "Rename"))
        .AddItem("Duplicate", kHierarchyCmdDuplicate, MenuItemFlag_None, EditMenuGroup::kDuplicatePriority, EditorIcons::kCopy,
                 CatalogShortcutLabel("Hierarchy", "Duplicate"))
        .AddItem("Delete", kHierarchyCmdDelete, MenuItemFlag_None, EditMenuGroup::kDeletePriority, EditorIcons::kTrash,
                 CatalogShortcutLabel("Hierarchy", "Delete"))
        .AddItem("---entity-edits-end", 0, MenuItemFlag_None, EditMenuGroup::kEndSeparatorPriority);
}

std::uint32_t HierarchyEntityCommandForKey(int key, int mods)
{
    if (MatchesCatalogShortcut("Hierarchy", "Rename", key, mods))
        return kHierarchyCmdRename;
    if (MatchesCatalogShortcut("Hierarchy", "Duplicate", key, mods))
        return kHierarchyCmdDuplicate;
    if (MatchesCatalogShortcut("Hierarchy", "Delete", key, mods))
        return kHierarchyCmdDelete;
    return 0;
}

} // namespace GameEngine::Editor
