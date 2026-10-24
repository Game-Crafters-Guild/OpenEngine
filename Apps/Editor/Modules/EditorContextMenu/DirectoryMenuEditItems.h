#pragma once

#include <cstdint>

namespace GameEngine
{
class ContextMenuBuilder;

// The edits on a folder right-clicked in the Assets folder tree, the folder-tree counterpart of
// the asset menu's Rename and Delete.
inline constexpr std::uint32_t kDirectoryCmdDelete = 0x3022;
inline constexpr std::uint32_t kDirectoryCmdRename = 0x3024;

// Adds Rename and Delete between two separators after a folder's Create submenu, the place the
// Hierarchy's entity menu gives its edits. Rename shows the key the folder tree binds to it.
void AddDirectoryEditItems(ContextMenuBuilder& builder);

} // namespace GameEngine
