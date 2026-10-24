#pragma once
#include <cstdint>

namespace GameEngine::Opaque
{
struct Handle
{
    std::uint32_t Id = 0;
};
} // namespace GameEngine::Opaque

namespace GameEngine::Components
{
struct FixtureBad
{
    float Speed = 0.0f;
    Opaque::Handle Data;
};
} // namespace GameEngine::Components
