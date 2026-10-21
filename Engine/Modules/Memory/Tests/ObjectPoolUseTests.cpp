// Who may use a Memory::ObjectPool<T>, and how misuse shows: serialized use from different
// threads, the Debug asserts (two users at once, a double or foreign Delete, New during the
// pool's destruction), the Release abort at the destruction of a pool whose slot was deleted
// twice, and AddressSanitizer's view of free and destroyed slots.

#include "Memory/ObjectPool.h"
#include "Memory/Testing/ScopedAllocationHold.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

namespace
{
using GameEngine::Memory::ObjectPool;
using GameEngine::Memory::Testing::ScopedAllocationHold;

/// 64 bytes: the first block holds 256 slots.
struct Request
{
    static int s_Destroyed;
    int Value = 0;
    std::array<std::byte, 60> Payload{};
    explicit Request(int value) : Value(value) {}
    ~Request() { ++s_Destroyed; }
};
int Request::s_Destroyed = 0;
static_assert(sizeof(Request) == 64);

/// The first block of an `ObjectPool<Request>`: 256 slots and one bit per slot.
constexpr std::size_t kRequestFirstBlockBytes = 256 * sizeof(Request) + 256 / 8;

/// One frame of a system that owns a pool: objects created and half of them deleted. The
/// first two frames also acquire blocks, so block acquisition changes thread as well.
void RunFrame(ObjectPool<Request>& pool, int frame)
{
    std::vector<Request*> requests;
    for (int index = 0; index < 600; ++index)
        requests.push_back(pool.New(frame));
    for (std::size_t index = 0; index < requests.size(); index += 2)
        pool.Delete(requests[index]);
    EXPECT_EQ(requests[1]->Value, frame);
}

/// Runs frame `frame` once the previous frame's thread has handed over, then hands over and
/// stays alive until every frame ran, so no two frames share a thread (a joined thread's id
/// can be reused by the next one).
void RunFrameAfterHandover(ObjectPool<Request>& pool, int frame, std::atomic<int>& framesDone, int frameCount)
{
    while (framesDone.load(std::memory_order_acquire) != frame)
        std::this_thread::yield();
    RunFrame(pool, frame);
    framesDone.store(frame + 1, std::memory_order_release);
    while (framesDone.load(std::memory_order_acquire) != frameCount)
        std::this_thread::yield();
}

/// Reads its sibling in its destructor, which the pool's destruction forbids once the sibling
/// has been destroyed.
struct ReadsItsSibling
{
    ReadsItsSibling* Sibling = nullptr;
    int Value = 7;
    ~ReadsItsSibling()
    {
        if (Sibling != nullptr)
        {
            [[maybe_unused]] const int read = static_cast<volatile int&>(Sibling->Value);
        }
    }
};

/// Creates an object of its own pool in its destructor.
struct NewsInItsDestructor
{
    ObjectPool<NewsInItsDestructor>& Pool;
    bool Spawn = true;
    explicit NewsInItsDestructor(ObjectPool<NewsInItsDestructor>& pool, bool spawn) : Pool(pool), Spawn(spawn) {}
    ~NewsInItsDestructor()
    {
        if (Spawn)
            Pool.New(Pool, false);
    }
};
} // namespace

// A system's member pool is used by whichever worker runs the system each frame; the frame's
// join orders the uses, so the pool must accept them without recording an owner thread.
TEST(ObjectPoolUseTests, APoolUsedBySystemsOnDifferentWorkersAcrossFramesDoesNotAssert)
{
    constexpr int kFrames = 3;
    ObjectPool<Request> pool("Tests.ObjectPool.AcrossWorkers");
    std::atomic<int> framesDone{0};
    std::vector<std::thread> workers;
    for (int frame = 0; frame < kFrames; ++frame)
        workers.emplace_back(RunFrameAfterHandover, std::ref(pool), frame, std::ref(framesDone), kFrames);
    for (std::thread& worker : workers)
        worker.join();
    EXPECT_EQ(pool.LiveCount(), static_cast<std::size_t>(kFrames * 300));
}

#if GE_DEBUG_INSTRUMENTATION && !defined(NDEBUG)
TEST(ObjectPoolUseDeathTest, DoubleDeleteAsserts)
{
    EXPECT_DEATH(
        {
            ObjectPool<Request> pool("Tests.ObjectPool.DoubleDelete");
            Request* request = pool.New(1);
            pool.New(2);
            pool.Delete(request);
            pool.Delete(request);
        },
        "this object was already returned to the pool");
}

TEST(ObjectPoolUseDeathTest, AForeignPointerAsserts)
{
    EXPECT_DEATH(
        {
            ObjectPool<Request> pool("Tests.ObjectPool.Foreign");
            pool.New(1);
            Request onTheStack{2};
            pool.Delete(&onTheStack);
        },
        "this pointer did not come from this pool");
}

TEST(ObjectPoolUseDeathTest, AMisalignedPointerAsserts)
{
    EXPECT_DEATH(
        {
            ObjectPool<Request> pool("Tests.ObjectPool.Misaligned");
            Request* request = pool.New(1);
            pool.Delete(reinterpret_cast<Request*>(reinterpret_cast<std::byte*>(request) + 8));
        },
        "this pointer did not come from this pool");
}

TEST(ObjectPoolUseDeathTest, NewDuringPoolDestructionAsserts)
{
    EXPECT_DEATH(
        {
            ObjectPool<NewsInItsDestructor> pool("Tests.ObjectPool.NewWhileDestroying");
            pool.New(pool, true);
        },
        "New during the pool's destruction");
}

// Thread 1 is parked inside the pool's first block acquisition; thread 2 creating an object in
// the same pool meanwhile is the overlap the serial-use word reports.
TEST(ObjectPoolUseDeathTest, TwoThreadsInsideNewAtOnceAssert)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            ObjectPool<Request> pool("Tests.ObjectPool.TwoAtOnce");
            std::atomic<ScopedAllocationHold*> published{nullptr};
            std::thread first([&pool, &published] {
                ScopedAllocationHold hold{kRequestFirstBlockBytes, 1};
                published.store(&hold, std::memory_order_release);
                pool.New(1); // parks in the first block's acquisition
            });
            ScopedAllocationHold* hold = nullptr;
            while ((hold = published.load(std::memory_order_acquire)) == nullptr)
                std::this_thread::yield();
            hold->WaitUntilHeld();
            pool.New(2);
            hold->Release();
            first.join();
        },
        "two threads used this ObjectPool at once");
}
#endif

#if !GE_DEBUG_INSTRUMENTATION
namespace
{
/// Ends the death test's child when the pool's destruction does not, so a destruction that
/// never returns fails the test with this line instead of hanging the suite.
void ExitAfterWatchdogDelay()
{
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::fprintf(stderr, "watchdog: the pool's destruction did not return\n");
    std::_Exit(1);
}
} // namespace

// Release keeps no per-slot bit on the hot path, so a double Delete is not refused there: it
// links the slot to itself. The pool's destruction bounds its freelist walk and names the cycle.
TEST(ObjectPoolUseDeathTest, DestroyingAPoolAfterADoubleDeleteAbortsInRelease)
{
    EXPECT_DEATH(
        {
            std::thread(ExitAfterWatchdogDelay).detach();
            ObjectPool<Request> pool("Tests.ObjectPool.ReleaseDoubleDelete");
            Request* request = pool.New(1);
            pool.New(2);
            pool.Delete(request);
            pool.Delete(request);
        },
        "ObjectPool: a slot was deleted twice; the freelist is a cycle");
}
#endif

#if defined(GE_MEMORY_ASAN)
// A free slot is poisoned except the word that holds the freelist link, so New's pop and the
// destruction's freelist walk read it without a report.
TEST(ObjectPoolUseTests, ChurnUnderAsanReportsNoFreelistRead)
{
    ObjectPool<Request> pool("Tests.ObjectPool.AsanChurn");
    std::vector<Request*> live;
    for (int index = 0; index < 600; ++index)
        live.push_back(pool.New(index));
    std::uint32_t state = 0x9E3779B9u;
    for (int round = 0; round < 2000; ++round)
    {
        state = state * 1664525u + 1013904223u;
        const std::size_t index = (state >> 8) % live.size();
        pool.Delete(live[index]);
        live[index] = pool.New(round);
    }
    for (std::size_t index = 0; index < live.size(); index += 2)
        pool.Delete(live[index]);
    EXPECT_EQ(pool.LiveCount(), 300u);
}

// The pool destroys the sibling at the lower address first and poisons its slot whole, so the
// later destructor's read of it is reported.
TEST(ObjectPoolUseDeathTest, AFreedSlotReadDuringPoolDestructionIsReportedUnderAsan)
{
    EXPECT_DEATH(
        {
            ObjectPool<ReadsItsSibling> pool("Tests.ObjectPool.AsanSiblingRead");
            ReadsItsSibling* sibling = pool.New();
            ReadsItsSibling* reader = pool.New();
            reader->Sibling = sibling;
        },
        "READ of size");
}
#endif
