#pragma once

#include <atomic>
#include <cassert>
#include <cstdio>

namespace GameEngine::Memory::Detail
{

#if GE_DEBUG_INSTRUMENTATION
/// The Debug and DebugFast check that an externally serialized allocator has one user at a
/// time: each state change marks the word busy for its duration and asserts it was free. It
/// records no thread, so uses on different threads ordered by a join or a lock pass; two uses
/// that overlap in time assert.
class SerialUseCheck
{
  public:
    /// `allocator` is the type the report names ("Arena", "ObjectPool").
    void Enter(const char* allocator)
    {
        const bool wasBusy = m_Busy.exchange(true, std::memory_order_acquire);
        if (wasBusy)
        {
            std::fprintf(stderr,
                         "two threads used this %s at once; an %s is externally serialized: one user at a time "
                         "(give each thread its own, or order the uses with a join or a lock)\n",
                         allocator, allocator);
            assert(false && "two threads used one externally serialized Memory allocator at once (named above)");
        }
    }

    void Exit() { m_Busy.store(false, std::memory_order_release); }

  private:
    std::atomic<bool> m_Busy{false};
};
#endif

} // namespace GameEngine::Memory::Detail
