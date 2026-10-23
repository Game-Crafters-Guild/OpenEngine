#pragma once
#include <cstdint>

namespace GameEngine::Components
{
inline constexpr std::uint32_t kSampleCount = 4;

struct FixtureBad
{
    float Samples[kSampleCount * 2] = {};
};
} // namespace GameEngine::Components
