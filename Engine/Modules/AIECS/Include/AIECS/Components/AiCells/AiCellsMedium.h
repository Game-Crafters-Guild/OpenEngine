#pragma once

#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

/// The memory of one agent in 20 cells of 4 bytes, for a unit with a target and a few timers.
/// An agent has at most one of AiCellsSmall, AiCellsMedium and AiCellsLarge.
struct AiCellsMedium
{
    static constexpr uint32 kCount = 20;

    std::array<uint32, kCount> Values{};
};

static_assert(sizeof(AiCellsMedium) == 80);
static_assert(std::is_trivially_copyable_v<AiCellsMedium>);

} // namespace GameEngine::Components
