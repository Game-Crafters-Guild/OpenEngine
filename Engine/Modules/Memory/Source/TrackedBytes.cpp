#include "Memory/TrackedBytes.h"

#include "StatsRegistry.h"

#include <algorithm>
#include <atomic>

namespace GameEngine::Memory
{

TrackedBytes::TrackedBytes(std::string_view name, Residency residency)
{
    Detail::RegisterStats(m_Record.Value, name, StatsKind::TrackedBytes, residency);
}

TrackedBytes::~TrackedBytes()
{
    Detail::UnregisterStats(m_Record.Value);
}

void TrackedBytes::Add(std::uint64_t bytes)
{
    m_Record.Value.Bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void TrackedBytes::Subtract(std::uint64_t bytes)
{
    std::atomic<std::uint64_t>& count = m_Record.Value.Bytes;
    std::uint64_t current = count.load(std::memory_order_relaxed);
    while (!count.compare_exchange_weak(current, current - std::min(current, bytes), std::memory_order_relaxed))
    {
    }
}

} // namespace GameEngine::Memory
