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
    Other::FixtureMode Mode;
};
} // namespace GameEngine::Components
