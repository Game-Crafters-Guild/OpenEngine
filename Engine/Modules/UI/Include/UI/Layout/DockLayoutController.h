#pragma once

#include <memory>
#include <span>
#include <string>

namespace GameEngine
{
class DockNode;
class DockingManager;

/// Edits docking layouts without making DockingManager responsible for
/// preset-specific cloning and pruning policy.
class DockLayoutController
{
  public:
    explicit DockLayoutController(DockingManager& docking);

    /// Clone a layout while dropping tabs for unregistered panels and branches
    /// that become empty. A tab named in `keptPanelIds` stays even when its
    /// panel is not registered yet: the dock shows it as a placeholder until
    /// the panel registers.
    std::unique_ptr<DockNode> CloneForRegisteredPanels(const DockNode* source,
                                                       std::span<const std::string> keptPanelIds) const;

    /// Replace the active docking layout with the registered-panel subset,
    /// plus the tabs named in `keptPanelIds`. Returns false without changing
    /// the current layout when nothing survives.
    bool ApplyForRegisteredPanels(const DockNode* source, std::span<const std::string> keptPanelIds);

  private:
    std::unique_ptr<DockNode> CloneForRegisteredPanelsRecursive(const DockNode* source,
                                                                std::span<const std::string> keptPanelIds) const;
    bool KeepPanel(const std::string& panelId, std::span<const std::string> keptPanelIds) const;

    DockingManager& m_Docking;
};

} // namespace GameEngine
