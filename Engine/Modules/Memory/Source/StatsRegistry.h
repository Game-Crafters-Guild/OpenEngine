#pragma once

#include "Memory/Residency.h"
#include "Memory/StatsKind.h"

#include <string_view>

namespace GameEngine::Memory::Detail
{

struct StatsRecord;

/// Copies `name` (truncated to `StatsRecord::kMaxNameLength`, asserted in Debug and DebugFast),
/// `kind` and `residency` into `record` and links it into the registry. Every owner that
/// registers calls this from its constructor; `record` must stay at its address until
/// `UnregisterStats`.
void RegisterStats(StatsRecord& record, std::string_view name, StatsKind kind, Residency residency);

/// Unlinks `record`; after it returns no snapshot reads the record.
void UnregisterStats(StatsRecord& record);

} // namespace GameEngine::Memory::Detail
