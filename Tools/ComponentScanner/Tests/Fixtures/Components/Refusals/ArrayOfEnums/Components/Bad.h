#pragma once
#include <cstdint>

namespace GameEngine::Components
{
enum class FixtureMode : std::uint8_t
{
    Idle,
    Busy,
};

struct FixtureBad
{
    FixtureMode Modes[2];
};
} // namespace GameEngine::Components
