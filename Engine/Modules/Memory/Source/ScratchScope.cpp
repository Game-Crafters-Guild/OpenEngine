// Memory::ScratchScope's thread state: one arena per thread, the open scopes' frames in
// chunks that never move, the resources `std::pmr` containers hold, the side lists of
// out-of-order allocations and the retention hysteresis.
//
// The state is reached through one trivially destructible thread_local (a pointer, non-null
// while the state is live, and an Unregistered/Exited word) and freed by a separate exit
// sentinel, so code that runs after the sentinel during thread or static destruction finds
// Exited and is served from the heap, never from a freed arena. None of it is a template, so
// Engine.dll's generated export table carries every entry point (decision 67 of the allocator
// design).

#include "Memory/ScratchScope.h"

#include "Memory/StatsRecord.h"
#include "Types/FalseSharing.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace GameEngine::Memory
{
namespace Detail
{
namespace
{

/// Open-scope frames per chunk of the thread's frame storage.
constexpr std::uint32_t kFramesPerChunk = 64;
/// Outermost closes per hysteresis window: a retained block is released only when it exceeds
/// twice the largest outermost need of at least the last this many closes.
constexpr std::uint32_t kScratchTrimWindowCloses = 1024;
/// Outermost closes between two publications of the stats and two hysteresis checks; divides
/// `kScratchTrimWindowCloses`. The closes between pay the reset alone (the pre-registered
/// fallback of the allocator design's ScratchBench rule (a)).
constexpr std::uint32_t kBookkeepingCloses = 64;
static_assert(kScratchTrimWindowCloses % kBookkeepingCloses == 0);
#if GE_DEBUG_INSTRUMENTATION
/// Resources handed out round-robin in Debug and DebugFast, skipping those open scopes hold, so a
/// container that outlived its scope holds a closed resource until the ring comes back round.
constexpr std::size_t kQuarantineResources = 256;
#endif

/// The calling thread's id as the OS names it, the id `Platform::GetThreadName` takes.
std::uint64_t CurrentOsThreadId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

} // namespace

/// One heap allocation made outside the arena, linked into the frame of the scope it belongs
/// to and freed when that scope closes. The payload follows the node at `Alignment`.
struct SideAllocation
{
    SideAllocation* Next = nullptr;
    std::size_t Alignment = 0;
};

/// The `std::pmr` face of one open scope. In Release one lives in each frame and is reused by
/// every scope opened at that depth; in Debug and DebugFast each open takes the next closed one
/// of a per-thread ring (the frame's own when every one is open) and marks it closed at the
/// scope's close.
class ScratchResource final : public std::pmr::memory_resource
{
  public:
    /// Null for the resource of a scope opened after the thread's state exited.
    ScratchThread* Thread = nullptr;
    ScratchFrame* Frame = nullptr;
    std::uint32_t Depth = 0;
#if GE_DEBUG_INSTRUMENTATION
    bool Closed = true;
#endif

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void*, std::size_t, std::size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

/// What one open scope needs beyond the scope object: where the arena stood when it opened
/// (unused by an outermost scope, whose close resets the arena), and its heap side list,
/// empty whenever no scope is open at the frame's depth.
struct ScratchFrame
{
    Arena::Marker Marker;
    SideAllocation* SideList = nullptr;
    ScratchResource Resource;
};

namespace
{

void* AllocateSide(ScratchFrame& frame, std::size_t bytes, std::size_t alignment)
{
    const std::size_t nodeAlignment = std::max(alignment, alignof(SideAllocation));
    const std::size_t header = (sizeof(SideAllocation) + nodeAlignment - 1) & ~(nodeAlignment - 1);
    if (bytes > std::numeric_limits<std::size_t>::max() - header)
        throw std::bad_alloc();
    auto* base = static_cast<std::byte*>(::operator new(header + bytes, std::align_val_t{nodeAlignment}));
    auto* node = ::new (base) SideAllocation{frame.SideList, nodeAlignment};
    frame.SideList = node;
    return base + header;
}

void FreeSideList(ScratchFrame& frame)
{
    for (SideAllocation* node = frame.SideList; node != nullptr;)
    {
        SideAllocation* next = node->Next;
        ::operator delete(static_cast<void*>(node), std::align_val_t{node->Alignment});
        node = next;
    }
    frame.SideList = nullptr;
}

} // namespace

/// One thread's scratch state. Created at the thread's first `ScratchScope`, destroyed by its
/// exit sentinel. Its own heap allocation, reached only through the thread's slot, never in an
/// array; a full separation of padding before and after its state keeps the arena's stats
/// record, written by the closes that publish, off every line a neighbouring allocation (another
/// thread's state among them) uses. Explicit padding, not alignas: an over-aligned class is
/// padded by the compiler, which MSVC reports as C4324, an error in this build.
class ScratchThread
{
  public:
    explicit ScratchThread(std::uint64_t osThreadId) : m_Arena(ScratchArenaTag{}, "Memory.Scratch", osThreadId)
    {
        InitializeFrames(m_FirstFrames, 0);
    }

    ~ScratchThread() { assert(m_OpenDepth == 0 && "a thread exited with a ScratchScope open"); }

    ScratchThread(const ScratchThread&) = delete;
    ScratchThread& operator=(const ScratchThread&) = delete;

    Arena& GetArena() { return m_Arena; }
    const std::uint32_t& OpenDepth() const { return m_OpenDepth; }

    /// Opens the frame at the current depth; returns the resource the scope hands out.
    ScratchResource& Open(ScratchFrame*& frame)
    {
        const std::uint32_t depth = m_OpenDepth;
        frame = &FrameAt(depth);
        assert(frame->SideList == nullptr && "a closed scratch frame keeps no side list");
        assert(frame->Resource.Depth == depth && "a scratch frame's own resource carries the frame's depth");
        if (depth != 0)
            frame->Marker = m_Arena.GetMarker();
#if GE_DEBUG_INSTRUMENTATION
        ScratchResource& resource = TakeQuarantineResource(*frame);
        resource.Thread = this;
        resource.Frame = frame;
        resource.Depth = depth;
        resource.Closed = false;
#else
        ScratchResource& resource = frame->Resource;
#endif
        m_OpenDepth = depth + 1;
        return resource;
    }

    /// The close of a scope with no side allocation that needs no bookkeeping: a nested close
    /// whose marker is in the current block, or the reset of a one-block arena between two
    /// bookkeeping closes. Calls nothing, so the scope's destructor stays a leaf; returns false,
    /// changing nothing, for every other close, which `Close` does.
    bool TryCloseSteady(std::uint32_t depth, ScratchFrame& frame, [[maybe_unused]] std::pmr::memory_resource& resource)
    {
        assert(m_OpenDepth == depth + 1 &&
               "a ScratchScope closed while a scope opened after it is still open; a ScratchScope is a local "
               "variable, never held in an object or on the heap");
        if (frame.SideList != nullptr)
            return false;
        if (depth != 0)
        {
            // No stats and no hysteresis: the outermost close folds this rewind's peak in.
            if (!m_Arena.TryRewindWithinCurrentBlock(frame.Marker))
                return false;
        }
        else
        {
            std::size_t peak = 0;
            if (m_ClosesSinceBookkeeping + 1 == kBookkeepingCloses || !m_Arena.TryResetOneBlock(peak))
                return false;
            ++m_ClosesSinceBookkeeping;
            m_WindowPeak = std::max(m_WindowPeak, peak);
        }
#if GE_DEBUG_INSTRUMENTATION
        static_cast<ScratchResource&>(resource).Closed = true;
#endif
        m_OpenDepth = depth;
        return true;
    }

    /// Every close `TryCloseSteady` refused (it checked the order).
    void Close(std::uint32_t depth, ScratchFrame& frame, [[maybe_unused]] std::pmr::memory_resource& resource)
    {
        FreeSideList(frame);
#if GE_DEBUG_INSTRUMENTATION
        static_cast<ScratchResource&>(resource).Closed = true;
#endif
        m_OpenDepth = depth;
        if (depth != 0)
        {
            m_Arena.RewindPublishing(frame.Marker, false);
            return;
        }
        ++m_ClosesSinceBookkeeping;
        CloseOutermost();
    }

    /// Counts an allocation through an open scope that is not the innermost one.
    void CountOutOfOrder()
    {
        Detail::StatsRecord& record = m_Arena.m_Record;
        record.OutOfOrder.store(record.OutOfOrder.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
#if GE_DEBUG_INSTRUMENTATION
        if (!m_OutOfOrderLogged)
        {
            m_OutOfOrderLogged = true;
            std::fprintf(stderr, "ScratchScope: an allocation through an outer scope while an inner scope was open "
                                 "was served from the heap; allocate from the innermost scope (logged once per "
                                 "thread)\n");
        }
#endif
    }

    ScratchFrame& FrameAt(std::uint32_t depth)
    {
        if (depth < kFramesPerChunk) [[likely]]
            return m_FirstFrames[depth];
        return FrameAfterFirstChunk(depth);
    }

  private:
    using FrameChunk = std::array<ScratchFrame, kFramesPerChunk>;

#if GE_DEBUG_INSTRUMENTATION
    /// The next closed resource of the ring; the frame's own when every one belongs to an open
    /// scope, so an open scope's resource is never handed to another. A container that outlived
    /// an earlier scope at that depth then grows unchecked.
    ScratchResource& TakeQuarantineResource(ScratchFrame& frame)
    {
        for (std::size_t probe = 0; probe < kQuarantineResources; ++probe)
        {
            const std::size_t slot = (m_QuarantineNext + probe) % kQuarantineResources;
            if (m_Quarantine[slot].Closed)
            {
                m_QuarantineNext = (slot + 1) % kQuarantineResources;
                return m_Quarantine[slot];
            }
        }
        return frame.Resource;
    }
#endif

    void InitializeFrames(FrameChunk& frames, std::uint32_t firstDepth)
    {
        for (std::uint32_t slot = 0; slot < kFramesPerChunk; ++slot)
        {
            ScratchFrame& frame = frames[slot];
            frame.Resource.Thread = this;
            frame.Resource.Frame = &frame;
            frame.Resource.Depth = firstDepth + slot;
        }
    }

    /// The frame at a depth of `kFramesPerChunk` or more, appending the chunk that holds it.
    ScratchFrame& FrameAfterFirstChunk(std::uint32_t depth)
    {
        const std::size_t chunk = depth / kFramesPerChunk - 1;
        if (chunk == m_Chunks.size())
        {
            m_Chunks.push_back(std::make_unique<FrameChunk>());
            InitializeFrames(*m_Chunks.back(), static_cast<std::uint32_t>((chunk + 1) * kFramesPerChunk));
        }
        return (*m_Chunks[chunk])[depth % kFramesPerChunk];
    }

    /// An outermost close that is not steady: resets the arena, coalescing a chain into one
    /// block. Every `kBookkeepingCloses`-th outermost close (the counter already advanced) also
    /// publishes the stats and runs the hysteresis: releasing a retained block more than twice
    /// the largest outermost need of the window.
    void CloseOutermost()
    {
        m_WindowPeak = std::max(m_WindowPeak, m_Arena.PeakBytesSinceReset());
        const bool bookkeeping = m_ClosesSinceBookkeeping == kBookkeepingCloses;
        m_Arena.ResetPublishing(bookkeeping);
        if (!bookkeeping)
            return;
        m_ClosesSinceBookkeeping = 0;
        Detail::StatsRecord& record = m_Arena.m_Record;
        // The window peak covers every close since the last bookkeeping, unpublished ones too.
        if (m_WindowPeak > record.HighWaterBytes.load(std::memory_order_relaxed))
            record.HighWaterBytes.store(m_WindowPeak, std::memory_order_relaxed);
        const std::size_t need = std::max(m_WindowPeak, m_PreviousWindowPeak);
        m_ClosesInWindow += kBookkeepingCloses;
        if (m_ClosesInWindow == kScratchTrimWindowCloses)
        {
            m_PreviousWindowPeak = m_WindowPeak;
            m_WindowPeak = 0;
            m_ClosesInWindow = 0;
        }
        const std::size_t retained = m_Arena.BytesReserved();
        if (retained <= kArenaFirstBlockBytes || retained <= 2 * need)
            return;
        m_Arena.ReleaseBlocks(std::max(need, kArenaFirstBlockBytes));
        record.Shrinks.store(record.Shrinks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    [[maybe_unused]] char m_LeadingSeparation[kFalseSharingSeparation];
    Arena m_Arena;
    std::uint32_t m_OpenDepth = 0;
    std::uint32_t m_ClosesInWindow = 0;
    std::uint32_t m_ClosesSinceBookkeeping = 0;
    /// The largest outermost need of the current and the previous hysteresis window.
    std::size_t m_WindowPeak = 0;
    std::size_t m_PreviousWindowPeak = 0;
    /// The frames of depths 0 to `kFramesPerChunk - 1`, reached without an indirection.
    FrameChunk m_FirstFrames;
    /// The deeper frames' chunks. Never shrinks before thread exit, so a frame's address is
    /// stable at any depth.
    std::vector<std::unique_ptr<FrameChunk>> m_Chunks;
#if GE_DEBUG_INSTRUMENTATION
    std::array<ScratchResource, kQuarantineResources> m_Quarantine;
    std::size_t m_QuarantineNext = 0;
    bool m_OutOfOrderLogged = false;
#endif
    [[maybe_unused]] char m_TrailingSeparation[kFalseSharingSeparation];
};

static_assert(alignof(ScratchThread) <= alignof(std::max_align_t),
              "ScratchThread must not be over-aligned: MSVC pads an over-aligned class and reports C4324");
static_assert(sizeof(ScratchThread) >= sizeof(Arena) + 2 * kFalseSharingSeparation,
              "ScratchThread keeps a full false-sharing separation before and after its state");

namespace
{

enum class ScratchThreadState : std::uint8_t
{
    /// The exit sentinel has not run. A null pointer means the thread has not opened a scope; a
    /// non-null one, that its state exists and the sentinel is registered.
    Unregistered,
    /// Its exit sentinel has run: scopes are served from the heap.
    Exited,
};

struct ScratchThreadSlot
{
    ScratchThread* Thread = nullptr;
    ScratchThreadState State = ScratchThreadState::Unregistered;
};

constinit thread_local ScratchThreadSlot t_Scratch;

/// Frees the thread's state at thread exit and marks it Exited. Registered with the thread by
/// the first scope, which names it.
struct ScratchExitSentinel
{
    ~ScratchExitSentinel()
    {
        t_Scratch.State = ScratchThreadState::Exited;
        delete std::exchange(t_Scratch.Thread, nullptr);
    }
};

thread_local ScratchExitSentinel t_ScratchExit;

/// The open-scope count a scope on an exited thread compares against: never its depth plus
/// one, so every allocation takes the heap path.
constexpr std::uint32_t kExitedOpenDepth = 0;
constexpr std::uint32_t kExitedDepth = 1;

/// The first scope of a thread creates its state; a scope on an exited thread gets null.
ScratchThread* RegisterScratchThread()
{
    ScratchThreadSlot& slot = t_Scratch;
    if (slot.State == ScratchThreadState::Exited)
        return nullptr;
    // Naming the sentinel registers its destructor with this thread.
    [[maybe_unused]] ScratchExitSentinel& sentinel = t_ScratchExit;
    slot.Thread = new ScratchThread(CurrentOsThreadId());
    return slot.Thread;
}

/// A non-null pointer is a live state: registration stores it after naming the exit sentinel,
/// and the sentinel clears it as it frees the state.
ScratchThread* AcquireScratchThread()
{
    ScratchThread* thread = t_Scratch.Thread;
    if (thread == nullptr) [[unlikely]]
        return RegisterScratchThread();
    return thread;
}

} // namespace

ScratchThread* PeekScratchThread()
{
    return t_Scratch.Thread;
}

void* ScratchResource::do_allocate(std::size_t bytes, std::size_t alignment)
{
#if GE_DEBUG_INSTRUMENTATION
    assert(!Closed && "a container outlived its ScratchScope; declare it inside the scope");
    assert(PeekScratchThread() == Thread &&
           "a ScratchScope allocates only on the thread that opened it; give other threads spans");
#endif
    if (Thread == nullptr)
        return AllocateSide(*Frame, bytes, alignment);
    const std::uint32_t open = Thread->OpenDepth();
    if (Depth + 1 == open)
        return Thread->GetArena().Allocate(bytes, alignment);
    if (Depth < open)
    {
        Thread->CountOutOfOrder();
        return AllocateSide(*Frame, bytes, alignment);
    }
    // Release only: a container that outlived its scope, growing while the depth is closed.
    // The memory comes from the arena and returns at the next outermost close.
    return Thread->GetArena().Allocate(bytes, alignment);
}

} // namespace Detail

ScratchScope::ScratchScope() : m_Thread(Detail::AcquireScratchThread())
{
    if (m_Thread == nullptr)
    {
        OpenOnExitedThread();
        return;
    }
    m_Arena = &m_Thread->GetArena();
    m_OpenDepth = &m_Thread->OpenDepth();
    m_Depth = *m_OpenDepth;
    m_Resource = &m_Thread->Open(m_Frame);
}

ScratchScope::~ScratchScope()
{
    if (m_Thread == nullptr || !m_Thread->TryCloseSteady(m_Depth, *m_Frame, *m_Resource))
        CloseUnsteady();
}

void ScratchScope::OpenOnExitedThread()
{
    auto* frame = new Detail::ScratchFrame();
    frame->Resource.Frame = frame;
#if GE_DEBUG_INSTRUMENTATION
    frame->Resource.Closed = false;
#endif
    m_Arena = nullptr;
    m_Frame = frame;
    m_Resource = &frame->Resource;
    m_OpenDepth = &Detail::kExitedOpenDepth;
    m_Depth = Detail::kExitedDepth;
}

void ScratchScope::CloseUnsteady()
{
    if (m_Thread == nullptr)
    {
        Detail::FreeSideList(*m_Frame);
        delete m_Frame;
        return;
    }
    m_Thread->Close(m_Depth, *m_Frame, *m_Resource);
}

void* ScratchScope::AllocateAside(std::size_t bytes, std::size_t alignment)
{
    if (m_Thread != nullptr)
        m_Thread->CountOutOfOrder();
    return Detail::AllocateSide(*m_Frame, bytes, alignment);
}

} // namespace GameEngine::Memory
