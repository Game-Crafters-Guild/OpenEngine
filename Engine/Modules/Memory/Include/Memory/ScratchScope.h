#pragma once

#include "Memory/Arena.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace GameEngine::Memory
{

namespace Detail
{
class ScratchThread;
struct ScratchFrame;

/// The calling thread's scratch state, or null when it has none (it never opened a scope, or
/// it has exited). Reads the state without creating it; the Debug opening-thread check.
ScratchThread* PeekScratchThread();
} // namespace Detail

/// Stack memory for one block of code: what it allocates lives until the scope closes, and
/// comes from the calling thread's own scratch arena, so a steady caller allocates nothing.
///
///     {
///         Memory::ScratchScope scratch;                          // declare it first
///         std::span<Candidate> candidates = scratch.AllocateArrayForOverwrite<Candidate>(count);
///         std::pmr::vector<int> open{&scratch.Resource()};       // containers after it
///         ...                                                    // fork units, give them spans, join
///     }                                                          // everything is returned here
///
/// - **Lifetime.** Allocations live until the closing brace. Scopes nest on a thread, last
///   opened first closed: an inner scope's allocations return at its close, the outer scope's
///   stay.
/// - **Threading.** Only the thread that opened the scope allocates from it (asserted in Debug
///   and DebugFast). Any thread may read and write the memory until the scope closes: give
///   units spans, never the scope or a container bound to it, and join before the scope closes.
///   Never across a `Submit`: its body may still run after the scope has closed.
/// - **Automatic storage only.** Declare it as a local; it cannot be copied, moved or created
///   with `new`. Debug and DebugFast assert that a scope closes as the innermost one.
/// - **Types.** `New` and the arrays take trivially destructible types only; a `std::pmr`
///   container on `Resource()` runs its own destructor. Declare containers after the scope, so
///   they are destroyed before it closes: Debug and DebugFast assert when a container that
///   outlived its scope grows. The check covers 256 scopes per thread: past 256 open at once, a
///   scope hands out its depth's own resource, and a container that outlived an earlier scope at
///   that depth is not caught.
/// - **Outer scopes.** An allocation through a scope that is not the innermost open one is
///   served from the heap and freed when that scope closes. It is correct but costs a heap
///   allocation; the scratch stats count it as out-of-order.
/// - **Retention.** A thread's arena starts at 64 KiB on its first allocation and keeps what
///   its outermost scopes need; a block more than twice the largest need of at least the last
///   1024 outermost scopes is released (checked every 64th outermost close, when the thread's
///   scratch stats are also published). A thread that never opens a scope allocates nothing.
/// - **Failure.** `std::bad_alloc` when memory cannot be acquired; never null.
class ScratchScope
{
  public:
    ScratchScope();
    ~ScratchScope();

    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    ScratchScope(ScratchScope&&) = delete;
    ScratchScope& operator=(ScratchScope&&) = delete;
    static void* operator new(std::size_t) = delete;
    static void* operator new[](std::size_t) = delete;

    /// `alignment` is a power of two.
    void* Allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));

    /// Constructs a trivially destructible `T` in the scope.
    template <class T, class... Args>
    T* New(Args&&... args);

    /// `count` value-initialised elements (zeroes for arithmetic types).
    template <class T>
    std::span<T> AllocateArray(std::size_t count);

    /// `count` elements left indeterminate, for arrays the caller fills before reading.
    template <class T>
    std::span<T> AllocateArrayForOverwrite(std::size_t count);

    /// A copy of `source` in the scope.
    template <class T>
    std::span<T> CopyArray(std::span<const T> source);

    /// A memory resource over this scope for `std::pmr` containers; deallocation does nothing
    /// (the memory returns when the scope closes).
    std::pmr::memory_resource& Resource() { return *m_Resource; }

  private:
    /// Every allocation the innermost-scope bump cannot serve: through an outer scope, or on a
    /// thread whose scratch state has exited.
    void* AllocateAside(std::size_t bytes, std::size_t alignment);
    void* AllocateArrayBytes(std::size_t count, std::size_t elementBytes, std::size_t alignment);
    /// Every close the destructor does not finish inline: one with a side allocation, a
    /// bookkeeping close, a close that needs the full rewind or reset, a scope on an exited thread.
    void CloseUnsteady();
    /// The constructor's path on a thread whose scratch state has exited: a heap frame of its own.
    void OpenOnExitedThread();

    // No default member initialisers: both constructor paths set every member, and defaults
    // would be stored ahead of the out-of-line calls only to be overwritten after them.

    /// Null on a thread whose scratch state has exited; the scope then serves every allocation
    /// from the heap.
    Detail::ScratchThread* m_Thread;
    /// Null on a thread whose scratch state has exited.
    Arena* m_Arena;
    /// The thread's count of open scopes. This scope is the innermost one when it equals
    /// `m_Depth + 1`; a scope on an exited thread points at a count that never does.
    const std::uint32_t* m_OpenDepth;
    std::uint32_t m_Depth;
    Detail::ScratchFrame* m_Frame;
    std::pmr::memory_resource* m_Resource;
};

inline void* ScratchScope::Allocate(std::size_t bytes, std::size_t alignment)
{
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0 && "ScratchScope alignment must be a power of two");
#if GE_DEBUG_INSTRUMENTATION
    assert(Detail::PeekScratchThread() == m_Thread &&
           "a ScratchScope allocates only on the thread that opened it; give other threads spans");
#endif
    if (*m_OpenDepth == m_Depth + 1)
        return m_Arena->Allocate(bytes, alignment);
    return AllocateAside(bytes, alignment);
}

inline void* ScratchScope::AllocateArrayBytes(std::size_t count, std::size_t elementBytes, std::size_t alignment)
{
    if (elementBytes != 0 && count > std::numeric_limits<std::size_t>::max() / elementBytes)
        throw std::bad_array_new_length();
    return Allocate(count * elementBytes, alignment);
}

template <class T, class... Args>
T* ScratchScope::New(Args&&... args)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "ScratchScope holds trivially destructible types; use a std::pmr container on Resource(), or an "
                  "Arena you own for objects with destructors");
    return ::new (Allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
}

template <class T>
std::span<T> ScratchScope::AllocateArray(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "ScratchScope arrays hold trivially destructible types; use a std::pmr container on Resource()");
    T* elements = static_cast<T*>(AllocateArrayBytes(count, sizeof(T), alignof(T)));
    std::uninitialized_value_construct_n(elements, count);
    return {elements, count};
}

template <class T>
std::span<T> ScratchScope::AllocateArrayForOverwrite(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "ScratchScope arrays hold trivially destructible types; use a std::pmr container on Resource()");
    T* elements = static_cast<T*>(AllocateArrayBytes(count, sizeof(T), alignof(T)));
    std::uninitialized_default_construct_n(elements, count);
    return {elements, count};
}

template <class T>
std::span<T> ScratchScope::CopyArray(std::span<const T> source)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "ScratchScope arrays hold trivially destructible types; use a std::pmr container on Resource()");
    T* elements = static_cast<T*>(AllocateArrayBytes(source.size(), sizeof(T), alignof(T)));
    std::uninitialized_copy(source.begin(), source.end(), elements);
    return {elements, source.size()};
}

} // namespace GameEngine::Memory
