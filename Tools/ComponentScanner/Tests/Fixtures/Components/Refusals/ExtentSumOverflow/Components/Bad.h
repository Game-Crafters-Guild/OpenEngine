#pragma once
#include <cstdint>

namespace GameEngine::Components
{
inline constexpr std::int64_t kFixtureHalf = 0x4000000000000000;

struct FixtureBad
{
    std::uint8_t Bytes[kFixtureHalf + kFixtureHalf];
};
} // namespace GameEngine::Components
