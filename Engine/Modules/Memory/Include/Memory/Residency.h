#pragma once

#include <cstdint>

namespace GameEngine::Memory
{

/// Where the bytes a stats record counts live.
enum class Residency : std::uint8_t
{
    /// Process memory.
    Cpu,
    /// Device memory, or a host-visible buffer the device reads.
    Gpu,
};

} // namespace GameEngine::Memory
