#pragma once

#include "Engine/Rendering/WorldDrawTypes.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::Engine::Renderer
{

/// One world's submission records, retained across frames and keyed by the
/// GPUScene instance each record draws, so a structural change of k instances
/// appends and erases records for those k instances instead of rebuilding all
/// of them.
///
/// The records stay contiguous (Records() is what SubmitWorldSubmissions takes).
/// Erase keeps the survivors' relative order and Append adds after every record
/// held, so the order stays nearly sorted for the compat draw lists' per-frame
/// sort, which is cheap only on such input. It is not the full lane's record
/// order after an edit: re-prepared and new records follow every survivor. Sorted
/// transparency breaks equal-depth ties on submission order, so such a tie can
/// draw in a different order after a delta frame than after a full lane.
class KeyedWorldSubmissions
{
  public:
    void Clear();

    /// Adds one record for `record.instanceIndex`, after every record held.
    void Append(const WorldSubmissionRecord& record);

    /// Removes every record of each instance in `instanceIndices` (one that holds
    /// none is skipped) in a single pass that keeps the survivors' order: O(k)
    /// when none of them holds records, O(records) otherwise.
    void Erase(std::span<const uint32> instanceIndices);

    std::span<const WorldSubmissionRecord> Records() const { return m_Records; }

    std::size_t Size() const { return m_Records.size(); }

  private:
    std::vector<WorldSubmissionRecord> m_Records;
    std::vector<uint32> m_RecordCount; // per instance index: the records it holds
    std::vector<uint8> m_Erasing;      // per instance index: marked by the Erase in progress
};

} // namespace GameEngine::Engine::Renderer
