#pragma once
#include <cstdint>

namespace GameEngine::Components
{
// @ge-engine-internal
struct FixtureSlot
{
    std::uint32_t Slot = 0;
};

struct FixtureBad
{
    FixtureSlot Slots[2];
};
} // namespace GameEngine::Components
