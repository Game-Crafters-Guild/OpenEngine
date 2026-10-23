#pragma once
#include <cstdint>

namespace GameEngine::Components::Detail
{
enum class FixturePulse : std::uint8_t
{
    Slow,
};
} // namespace GameEngine::Components::Detail

namespace GameEngine::Components
{
struct FixturePulse
{
    float Rate = 0.0f;
};

struct FixtureBad
{
    Detail::FixturePulse Pulse;
};
} // namespace GameEngine::Components
