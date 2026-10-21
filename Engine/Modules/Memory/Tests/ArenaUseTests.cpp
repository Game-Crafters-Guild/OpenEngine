// Who may use a Memory::Arena, and when: serialized use from different threads, constructors
// and destructors that use the arena they live in, the Debug check for two users at once, and
// AddressSanitizer's view of memory a Reset took back.

#include "Memory/Arena.h"
#include "Memory/Testing/ScopedAllocationHold.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace
{
using GameEngine::Memory::Arena;
using GameEngine::Memory::Testing::ScopedAllocationHold;

struct Counted
{
    static int s_Destroyed;
    ~Counted() { ++s_Destroyed; }
};
int Counted::s_Destroyed = 0;

/// Allocates from the arena it lives in, in its constructor and in its destructor.
struct UsesItsArena
{
    static int s_Destroyed;
    Arena& Owner;
    int* Inner = nullptr;

    explicit UsesItsArena(Arena& owner) : Owner(owner)
    {
        Inner = Owner.New<int>(7);
        Owner.New<Counted>();
    }
    ~UsesItsArena()
    {
        Owner.New<Counted>();
        ++s_Destroyed;
    }
};
int UsesItsArena::s_Destroyed = 0;

/// One frame of a system that owns an arena it never resets: an allocation, a new block (so
/// block acquisition runs on this thread too) and an object.
void RunFrame(Arena& arena, int frame)
{
    int* value = arena.New<int>(frame);
    arena.Allocate(64 * 1024);
    arena.New<Counted>();
    EXPECT_EQ(*value, frame);
}

/// Runs frame `frame` once the previous frame's thread has handed over, then hands over and
/// stays alive until every frame ran, so no two frames share a thread (a joined thread's id
/// can be reused by the next one).
void RunFrameAfterHandover(Arena& arena, int frame, std::atomic<int>& framesDone, int frameCount)
{
    while (framesDone.load(std::memory_order_acquire) != frame)
        std::this_thread::yield();
    RunFrame(arena, frame);
    framesDone.store(frame + 1, std::memory_order_release);
    while (framesDone.load(std::memory_order_acquire) != frameCount)
        std::this_thread::yield();
}
} // namespace

// A system's member arena is used by whichever worker runs the system each frame; the frame's
// join orders the uses, so the arena must accept them without recording an owner thread.
TEST(ArenaUseTests, AnArenaNotResetUsedFromDifferentWorkersAcrossFramesDoesNotAssert)
{
    Counted::s_Destroyed = 0;
    {
        constexpr int kFrames = 3;
        Arena arena("Tests.Arena.AcrossWorkers", 4096);
        std::atomic<int> framesDone{0};
        std::vector<std::thread> workers;
        for (int frame = 0; frame < kFrames; ++frame)
            workers.emplace_back(RunFrameAfterHandover, std::ref(arena), frame, std::ref(framesDone), kFrames);
        for (std::thread& worker : workers)
            worker.join();
    }
    EXPECT_EQ(Counted::s_Destroyed, 3);
}

TEST(ArenaUseTests, ANewWhoseConstructorAllocatesFromTheSameArenaDoesNotAssert)
{
    Counted::s_Destroyed = 0;
    UsesItsArena::s_Destroyed = 0;
    {
        Arena arena("Tests.Arena.NestedNew");
        const UsesItsArena* outer = arena.New<UsesItsArena>(arena);
        EXPECT_EQ(*outer->Inner, 7);
        EXPECT_EQ(arena.GetMarker().DestructorCount, 2u);
    }
    EXPECT_EQ(UsesItsArena::s_Destroyed, 1);
    EXPECT_EQ(Counted::s_Destroyed, 2) << "the Counted made by the constructor and by the destructor";
}

// A destructor run by Rewind records a destructor of its own; the same walk runs it, exactly
// once, and the memory it took is rewound with the rest.
TEST(ArenaUseTests, ADestructorThatAllocatesDuringRewindHasItsRecordRun)
{
    Counted::s_Destroyed = 0;
    UsesItsArena::s_Destroyed = 0;
    Arena arena("Tests.Arena.DestructorAllocates");
    arena.Allocate(32);
    const Arena::Marker marker = arena.GetMarker();
    const std::size_t usedAtMarker = arena.BytesUsed();
    arena.New<UsesItsArena>(arena);

    arena.Rewind(marker);
    EXPECT_EQ(UsesItsArena::s_Destroyed, 1);
    EXPECT_EQ(Counted::s_Destroyed, 2) << "the Counted the destructor made was never destroyed, or twice";
    EXPECT_EQ(arena.GetMarker().DestructorCount, marker.DestructorCount);
    EXPECT_EQ(arena.BytesUsed(), usedAtMarker);

    arena.Reset();
    EXPECT_EQ(Counted::s_Destroyed, 2);
}

#if GE_DEBUG_INSTRUMENTATION && !defined(NDEBUG)
// Thread 1 is parked inside the arena's block acquisition; thread 2 allocating from the same
// arena meanwhile is the overlap the serial-use word reports.
TEST(ArenaUseDeathTest, TwoThreadsInsideOneArenaAtOnceAssert)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    constexpr std::size_t kBlockBytes = 96 * 1024;
    EXPECT_DEATH(
        {
            Arena arena("Tests.Arena.TwoAtOnce", kBlockBytes);
            std::atomic<ScopedAllocationHold*> published{nullptr};
            std::thread first([&arena, &published] {
                ScopedAllocationHold hold{kBlockBytes, 1};
                published.store(&hold, std::memory_order_release);
                arena.Allocate(16); // parks in the first block's acquisition
            });
            ScopedAllocationHold* hold = nullptr;
            while ((hold = published.load(std::memory_order_acquire)) == nullptr)
                std::this_thread::yield();
            hold->WaitUntilHeld();
            arena.Allocate(16);
            hold->Release();
            first.join();
        },
        "two threads used this Arena at once");
}
#endif

#if defined(GE_MEMORY_ASAN)
TEST(ArenaUseDeathTest, ReadAfterResetIsReportedUnderAsan)
{
    EXPECT_DEATH(
        {
            Arena arena("Tests.Arena.ReadAfterReset");
            volatile int* value = arena.New<int>(5);
            arena.Reset();
            [[maybe_unused]] const int read = *value;
        },
        "READ of size");
}

namespace
{
/// A 1-byte-aligned request followed by a 16-byte-aligned one leaves a poisoned alignment gap
/// between them, inside the range `Reset` and `Rewind` take back.
void AllocateAcrossAnAlignmentGap(Arena& arena)
{
    ASSERT_NE(arena.Allocate(5, 1), nullptr);
    ASSERT_NE(arena.Allocate(24, 16), nullptr);
}
} // namespace

TEST(ArenaUseTests, ResetOverAnAlignmentGapIsCleanUnderAsan)
{
    Arena arena("Tests.Arena.ResetOverAGap");
    AllocateAcrossAnAlignmentGap(arena);
    arena.Reset();
    EXPECT_EQ(arena.BytesUsed(), 0u);
}

TEST(ArenaUseTests, RewindOverAnAlignmentGapIsCleanUnderAsan)
{
    Arena arena("Tests.Arena.RewindOverAGap");
    const Arena::Marker start = arena.GetMarker();
    AllocateAcrossAnAlignmentGap(arena);
    arena.Rewind(start);
    EXPECT_EQ(arena.BytesUsed(), 0u);
}
#endif
