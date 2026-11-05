#pragma once

#include "UndoRedo/History/EditChangeKind.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
struct HistorySnapshotTarget
{
    using Snapshot = std::vector<std::uint8_t>;
    std::string debugLabel;
    // Document, generation, component and ordered entity identities. The action label identifies the property.
    std::vector<std::uint64_t> Identity;
    std::function<bool()> Validate;
    std::function<bool(Snapshot&)> Capture;
    // Validate every destination before writing. Failure leaves all destinations unchanged.
    std::function<bool(const Snapshot&)> Apply;
    std::function<void(EditChangeKind)> Notify;
    std::function<std::string(const Snapshot&, const Snapshot&)> Describe;
};
} // namespace GameEngine::Editor
