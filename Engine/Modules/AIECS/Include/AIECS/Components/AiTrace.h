#pragma once

#include "AIECS/AiTraceRecord.h"
#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

/// The last decisions of one agent, in every build. AiDecideSystem writes one record per decision
/// of an agent that has this component. A game adds it to the agents it traces and removes it
/// when it stops; one trace takes about 2.5 KB.
struct AiTrace
{
    static constexpr uint32 kCapacity = 64;

    /// A ring: record n is at Records[n % kCapacity], and the newest is record Count - 1.
    std::array<AIECS::AiTraceRecord, kCapacity> Records{};
    /// The records written since the component was added.
    uint32 Count = 0;
};

static_assert(std::is_trivially_copyable_v<AiTrace>);

} // namespace GameEngine::Components
