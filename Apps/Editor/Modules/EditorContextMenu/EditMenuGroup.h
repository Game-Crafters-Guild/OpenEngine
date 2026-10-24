#pragma once

namespace GameEngine
{

// The edit rows (Rename, Duplicate, Delete) of the Hierarchy's entity menu, the Assets folder
// tree's menu and the asset item menu, between two separators. Menus sort rows by (priority,
// path): these sit after the Create submenu (-1000) and the open rows, before a menu's own rows
// (0 and up), so every menu that has the edits shows them in the same place.
namespace EditMenuGroup
{
inline constexpr int kSeparatorPriority = -53;
inline constexpr int kRenamePriority = -52;
inline constexpr int kDuplicatePriority = -51;
inline constexpr int kDeletePriority = -50;
inline constexpr int kEndSeparatorPriority = -49;
} // namespace EditMenuGroup

} // namespace GameEngine
