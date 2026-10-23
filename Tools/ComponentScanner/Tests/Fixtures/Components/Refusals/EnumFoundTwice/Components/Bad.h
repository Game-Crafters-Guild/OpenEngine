#pragma once
#include <cstdint>

namespace GameEngine::Components
{
enum class FixtureMode : std::uint8_t
{
    Idle,
};

struct FixtureBad
{
    FixtureMode Mode;
};
} // namespace GameEngine::Components
