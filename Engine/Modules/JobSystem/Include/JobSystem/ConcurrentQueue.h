#pragma once

#include <cstddef>
#include <new>

// Lock-free MPMC queue (the vcpkg concurrentqueue port). Some versions of the
// header trigger MSVC /analyze diagnostics C6326 (constant-vs-constant
// comparison) and C6011, which are benign for this use; they are suppressed
// around the include only, so the module's own code keeps every warning.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 6326 6011)
#endif

#if __has_include(<concurrentqueue/moodycamel/concurrentqueue.h>)
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#elif __has_include(<concurrentqueue/concurrentqueue.h>)
#include <concurrentqueue/concurrentqueue.h>
#elif __has_include(<concurrentqueue.h>)
#include <concurrentqueue.h>
#else
#error "moodycamel concurrentqueue headers not found - ensure vcpkg concurrentqueue package is installed"
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace JobSystem
{

/// moodycamel traits that take the queue's memory (its blocks, block indices,
/// producers and implicit-producer hash) from the global `operator new`, so the
/// allocation counter and the process heap policy see it like every other
/// engine allocation. The default traits call `std::malloc`, which neither sees.
///
/// The `nothrow` form keeps moodycamel's contract: a failed allocation returns
/// null and the enqueue reports failure instead of throwing. moodycamel aligns
/// over-aligned blocks inside the buffer itself, so it only needs
/// `max_align_t` alignment from `malloc`, which the default `operator new`
/// alignment covers (asserted below).
struct QueueAllocationTraits : moodycamel::ConcurrentQueueDefaultTraits
{
    static void* malloc(std::size_t size) { return ::operator new(size, std::nothrow); }
    static void free(void* pointer) { ::operator delete(pointer); }
};

static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= alignof(moodycamel::details::max_align_t),
              "moodycamel needs max_align_t alignment from its traits' malloc");

/// The queue every JobSystem structure uses: moodycamel's, allocating through
/// `QueueAllocationTraits`.
template <typename T>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T, QueueAllocationTraits>;

/// Traits for a freelist that keeps at most `Capacity` entries and takes them by tokenless
/// `try_enqueue`. Each releasing thread fills its own sub-queue, and `try_enqueue` never grows
/// that sub-queue's block index. This index covers the whole capacity, so one thread can fill it.
template <std::size_t Capacity>
struct FreelistQueueTraits : QueueAllocationTraits
{
    static constexpr std::size_t IMPLICIT_INITIAL_INDEX_SIZE = Capacity / QueueAllocationTraits::BLOCK_SIZE;
};

/// A freelist queue. Construct it with `Capacity`: `try_enqueue` takes no new blocks, so the
/// capacity is the most entries it keeps.
template <typename T, std::size_t Capacity>
using FreelistQueue = moodycamel::ConcurrentQueue<T, FreelistQueueTraits<Capacity>>;

} // namespace JobSystem
