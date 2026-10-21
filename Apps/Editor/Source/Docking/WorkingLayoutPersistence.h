#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace GameEngine
{
class DockNode;
class DockingManager;

namespace Editor
{

/// Keeps the working dock layout per user and per project, so the editor
/// reopens with the panels where the user left them.
///
/// The layout is stored in the user's settings for the project
/// (`<user data>/Projects/<workspace id>/UserSettings.json`, key
/// `ui.workingLayout`), never in the project's shared settings: the named
/// layout presets live there and travel with the project.
class WorkingLayoutPersistence
{
  public:
    /// Stores `layout` as the working layout of the project at
    /// `workspaceRoot`. A tab whose panel neither `docking` nor the package
    /// panel registry knows is left out. `separateWindowPanelIds` names the
    /// panels shown in their own windows: the dock tree does not hold them, so
    /// the next restore leaves them closed and says so in the log.
    static void Save(const std::filesystem::path& workspaceRoot,
                     const DockNode* layout,
                     DockingManager& docking,
                     std::span<const std::string> separateWindowPanelIds);

    /// The saved working layout of the project at `workspaceRoot`, reduced to
    /// the panels `docking` has. Null when nothing is saved or the saved
    /// layout cannot be used; the caller then keeps the active preset's
    /// layout. Every reason other than "nothing saved" is one log line.
    ///
    /// A saved layout that names a built-in panel `docking` does not have, or
    /// names a panel twice, is not used at all. A package panel that is not
    /// registered yet keeps its tab as a placeholder, which the panel binds to
    /// when its package registers it.
    static std::unique_ptr<DockNode> Load(const std::filesystem::path& workspaceRoot,
                                          DockingManager& docking);
};

} // namespace Editor
} // namespace GameEngine
