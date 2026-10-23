#pragma once

#include <cstdint>

namespace GameEngine
{
class ContextMenuBuilder;
}

namespace GameEngine::Editor
{

// The Hierarchy's edits on its selected entities. Its context menu and its key map name the same
// ids and the panel runs both through one handler, so a menu entry and its key are one command.
inline constexpr std::uint32_t kHierarchyCmdDuplicate = 0x4160;
inline constexpr std::uint32_t kHierarchyCmdDelete = 0x4161;
inline constexpr std::uint32_t kHierarchyCmdRename = 0x4162;

// Adds Rename, Duplicate and Delete to the menu of a right-clicked entity, each showing the key the
// Hierarchy's key map binds to it.
void AddHierarchyEntityEditItems(ContextMenuBuilder& builder);

// The entity edit a key press asks for while the Hierarchy has focus: the Shortcuts catalog's
// Hierarchy bindings (F2 renames, Delete deletes, Ctrl/Cmd+D duplicates). 0 for any other key.
std::uint32_t HierarchyEntityCommandForKey(int key, int mods);

} // namespace GameEngine::Editor
