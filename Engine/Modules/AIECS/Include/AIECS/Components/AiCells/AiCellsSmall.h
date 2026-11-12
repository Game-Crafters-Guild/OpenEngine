#pragma once

#include "Types/Types.h"

#include <array>
#include <type_traits>

namespace GameEngine::Components
{

/// The memory of one agent in 8 cells of 4 bytes, for an animal or a simple unit. An agent has at
/// most one of AiCellsSmall, AiCellsMedium and AiCellsLarge; its program states which cells it uses.
/// Its tasks read and write it through AIECS::AiTaskContext::GetCells.
struct AiCellsSmall
{
    static constexpr uint32 kCount = 8;

    std::array<uint32, kCount> Values{};
};

static_assert(sizeof(AiCellsSmall) == 32);
static_assert(std::is_trivially_copyable_v<AiCellsSmall>);

} // namespace GameEngine::Components
