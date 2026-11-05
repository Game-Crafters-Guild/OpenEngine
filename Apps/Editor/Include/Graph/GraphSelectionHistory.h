#pragma once

#include "Types/FlatSet.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace GameEngine::Editor
{
/// Keeps graph history action boundaries aligned with the selected node set.
class GraphSelectionHistory
{
public:
    /// Return false if the pending action cannot be settled before selection changes.
    bool UpdateSelection(const std::unordered_set<std::string>& selectedNodeIds, UndoRedoService* history)
    {
        const bool unchanged = selectedNodeIds.size() == m_SelectedNodeIds.Size() &&
            std::all_of(selectedNodeIds.begin(), selectedNodeIds.end(),
                [this](const std::string& nodeId) { return m_SelectedNodeIds.Contains(nodeId); });
        if (unchanged)
            return true;
        if (history && !history->CommitPendingEdit())
            return false;
        m_SelectedNodeIds.Clear();
        m_SelectedNodeIds.Reserve(selectedNodeIds.size());
        for (const auto& nodeId : selectedNodeIds)
            m_SelectedNodeIds.Insert(nodeId);
        return true;
    }

private:
    FlatSet<std::string> m_SelectedNodeIds;
};
} // namespace GameEngine::Editor
