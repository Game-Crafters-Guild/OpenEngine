#pragma once

#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

/// The memory of one agent in 64 cells of 4 bytes, for a commander or a first-person enemy.
/// An agent has at most one of AiCellsSmall, AiCellsMedium and AiCellsLarge.
struct AiCellsLarge
{
    static constexpr uint32 kCount = 64;

    std::array<uint32, kCount> Values{};
};

static_assert(sizeof(AiCellsLarge) == 256);
static_assert(std::is_trivially_copyable_v<AiCellsLarge>);

} // namespace GameEngine::Components
