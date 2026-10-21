#include "UI/Layout/DockLayoutController.h"

#include "UI/Layout/Docking.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{

DockLayoutController::DockLayoutController(DockingManager& docking)
    : m_Docking(docking)
{
}

bool DockLayoutController::KeepPanel(const std::string& panelId,
                                     std::span<const std::string> keptPanelIds) const
{
    return m_Docking.GetPanel(panelId) != nullptr ||
           std::find(keptPanelIds.begin(), keptPanelIds.end(), panelId) != keptPanelIds.end();
}

std::unique_ptr<DockNode> DockLayoutController::CloneForRegisteredPanels(
    const DockNode* source, std::span<const std::string> keptPanelIds) const
{
    return CloneForRegisteredPanelsRecursive(source, keptPanelIds);
}

bool DockLayoutController::ApplyForRegisteredPanels(const DockNode* source,
                                                    std::span<const std::string> keptPanelIds)
{
    auto sanitized = CloneForRegisteredPanels(source, keptPanelIds);
    if (!sanitized)
        return false;

    m_Docking.SetRoot(std::move(sanitized));
    return true;
}

std::unique_ptr<DockNode> DockLayoutController::CloneForRegisteredPanelsRecursive(
    const DockNode* source, std::span<const std::string> keptPanelIds) const
{
    if (!source)
        return nullptr;

    if (source->IsLeaf())
    {
        auto result = DockNode::MakeLeaf();
        for (const auto& panelId : source->GetTabs())
        {
            if (KeepPanel(panelId, keptPanelIds))
                result->AddTab(panelId);
        }

        const std::string& active = source->GetActivePanelId();
        if (!active.empty() && KeepPanel(active, keptPanelIds))
            (void)result->ActivateTab(active);

        if (result->GetTabs().empty())
            return nullptr;
        return result;
    }

    auto first = CloneForRegisteredPanelsRecursive(source->First(), keptPanelIds);
    auto second = CloneForRegisteredPanelsRecursive(source->Second(), keptPanelIds);
    if (!first)
        return second;
    if (!second)
        return first;

    auto result = std::make_unique<DockNode>();
    result->SetSplit(source->GetSplitDirection(),
                     source->GetSplitRatio(),
                     std::move(first),
                     std::move(second));
    result->SetMinChildSizes(source->GetMinFirstPx(), source->GetMinSecondPx());
    return result;
}

} // namespace GameEngine
