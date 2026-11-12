#pragma once

#include "AIECS/AiPathTraceRecord.h"
#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

/// The last decisions of one state-tree agent, in every build. AiDecideSystem writes one record per
/// decision of a state-tree agent that has this component. A game adds it to the agents it traces and
/// removes it when it stops; one trace takes about 1.5 KB.
struct AiPathTrace
{
    static constexpr uint32 kCapacity = 64;

    /// A ring: record n is at Records[n % kCapacity], and the newest is record Count - 1.
    std::array<AIECS::AiPathTraceRecord, kCapacity> Records{};
    /// The records written since the component was added.
    uint32 Count = 0;
};

static_assert(std::is_trivially_copyable_v<AiPathTrace>);

} // namespace GameEngine::Components
