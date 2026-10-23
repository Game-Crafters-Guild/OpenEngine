#pragma once
#include <cstdint>

namespace GameEngine::Components
{
enum class FixtureMode
{
    Idle,
    Busy,
};

struct FixtureBad
{
    FixtureMode Mode;
};
} // namespace GameEngine::Components
