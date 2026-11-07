#pragma once

#include "Types/FlatSet.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <concepts>
#include <ranges>
#include <string>

namespace GameEngine::Editor
{
/// Keeps graph history action boundaries aligned with the selected node set.
class GraphSelectionHistory
{
public:
    /// Return false if the pending action cannot be settled before selection changes.
    /// Selected node IDs must be unique; their order does not affect action boundaries.
    template <typename TNodeIds>
        requires std::ranges::forward_range<const TNodeIds&> &&
                 std::ranges::sized_range<const TNodeIds&> &&
                 std::same_as<std::ranges::range_value_t<const TNodeIds&>, std::string>
    bool UpdateSelection(const TNodeIds& selectedNodeIds, UndoRedoService* history)
    {
        const auto selectedCount = std::ranges::size(selectedNodeIds);
        const bool unchanged = selectedCount == m_SelectedNodeIds.Size() &&
            std::ranges::all_of(selectedNodeIds,
                [this](const std::string& nodeId) { return m_SelectedNodeIds.Contains(nodeId); });
        if (unchanged)
            return true;
        if (history && !history->CommitPendingEdit())
            return false;
        m_SelectedNodeIds.Clear();
        m_SelectedNodeIds.Reserve(selectedCount);
        for (const auto& nodeId : selectedNodeIds)
            m_SelectedNodeIds.Insert(nodeId);
        return true;
    }

private:
    FlatSet<std::string> m_SelectedNodeIds;
};
} // namespace GameEngine::Editor
