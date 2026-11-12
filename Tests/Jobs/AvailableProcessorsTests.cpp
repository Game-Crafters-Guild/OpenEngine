#include <gtest/gtest.h>

#include "JobSystem/AvailableProcessors.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#  include <sched.h>
#endif

using JobSystem::AvailableProcessors;
using JobSystem::uint32;
using JobSystem::uint64;

namespace
{
constexpr uint32 kMachineProcessors = 32;

#if defined(_WIN32)
// Narrows the process to the lowest processor it may run on, and restores its mask at the end of the scope.
class AffinityNarrowedToOneProcessor
{
public:
    AffinityNarrowedToOneProcessor()
    {
        DWORD_PTR systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &m_SavedMask, &systemMask) == 0)
            return;
        // Both masks read 0 when the process has threads in more than one processor group.
        m_SpansProcessorGroups = m_SavedMask == 0;
        if (m_SpansProcessorGroups)
            return;
        const DWORD_PTR lowestProcessor = DWORD_PTR{1} << std::countr_zero(m_SavedMask);
        m_Narrowed = SetProcessAffinityMask(GetCurrentProcess(), lowestProcessor) != 0;
    }

    ~AffinityNarrowedToOneProcessor()
    {
        if (m_Narrowed)
            SetProcessAffinityMask(GetCurrentProcess(), m_SavedMask);
    }

    bool IsNarrowed() const { return m_Narrowed; }
    bool SpansProcessorGroups() const { return m_SpansProcessorGroups; }

private:
    DWORD_PTR m_SavedMask = 0;
    bool m_Narrowed = false;
    bool m_SpansProcessorGroups = false;
};
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
// Narrows the calling thread to the lowest processor it may run on, and restores its mask at the end of the scope.
// Query reads the mask of the calling thread, and the threads it starts inherit that mask.
class AffinityNarrowedToOneProcessor
{
public:
    AffinityNarrowedToOneProcessor()
    {
        if (sched_getaffinity(0, sizeof(m_SavedMask), &m_SavedMask) != 0)
            return;
        // A read that succeeds holds at least one processor.
        int lowestProcessor = 0;
        while (!CPU_ISSET(lowestProcessor, &m_SavedMask))
            ++lowestProcessor;
        cpu_set_t narrowedMask;
        CPU_ZERO(&narrowedMask);
        CPU_SET(lowestProcessor, &narrowedMask);
        m_Narrowed = sched_setaffinity(0, sizeof(narrowedMask), &narrowedMask) == 0;
    }

    ~AffinityNarrowedToOneProcessor()
    {
        if (m_Narrowed)
            sched_setaffinity(0, sizeof(m_SavedMask), &m_SavedMask);
    }

    bool IsNarrowed() const { return m_Narrowed; }

private:
    cpu_set_t m_SavedMask{};
    bool m_Narrowed = false;
};
#endif
} // namespace

// A process started with `start /affinity 0xFFFF` may run on 16 of the machine's 32 processors.
TEST(AvailableProcessors, MaskThatAllowsHalfTheMachineGivesTheMaskCount)
{
    const std::array<uint64, 1> mask = {0xFFFFull};
    const AvailableProcessors available = AvailableProcessors::FromAffinityMask(mask, kMachineProcessors);
    EXPECT_EQ(available.Count, 16u);
    EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::AffinityMask);
}

TEST(AvailableProcessors, MaskThatAllowsOneProcessorGivesOne)
{
    const std::array<uint64, 1> mask = {uint64{1} << 7};
    const AvailableProcessors available = AvailableProcessors::FromAffinityMask(mask, kMachineProcessors);
    EXPECT_EQ(available.Count, 1u);
    EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::AffinityMask);
}

// The count never passes the machine, and a mask that allows every processor limits nothing.
TEST(AvailableProcessors, MaskThatAllowsTheMachineOrMoreGivesTheMachine)
{
    const std::array<uint64, 1> wholeMachine = {(uint64{1} << kMachineProcessors) - 1};
    const std::array<uint64, 1> oneMore = {(uint64{1} << (kMachineProcessors + 1)) - 1};
    for (const std::array<uint64, 1>& mask : {wholeMachine, oneMore})
    {
        const AvailableProcessors available = AvailableProcessors::FromAffinityMask(mask, kMachineProcessors);
        EXPECT_EQ(available.Count, kMachineProcessors);
        EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::Machine);
    }
}

// An empty mask is a failed read, and gives the machine.
TEST(AvailableProcessors, EmptyMaskGivesTheMachine)
{
    const std::array<uint64, 2> zeroWords = {0, 0};
    for (const std::span<const uint64> mask : {std::span<const uint64>{}, std::span<const uint64>{zeroWords}})
    {
        const AvailableProcessors available = AvailableProcessors::FromAffinityMask(mask, kMachineProcessors);
        EXPECT_EQ(available.Count, kMachineProcessors);
        EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::Machine);
    }
}

// A Linux mask holds processors 64 and above in its later words.
TEST(AvailableProcessors, MaskCountsTheProcessorsOfEveryWord)
{
    const std::array<uint64, 2> mask = {0xFull, 0xF0ull};
    const AvailableProcessors available = AvailableProcessors::FromAffinityMask(mask, 128);
    EXPECT_EQ(available.Count, 8u);
    EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::AffinityMask);
}

// A process narrowed to one processor gets one available processor, and its default pool starts one worker.
TEST(AvailableProcessors, QueryReadsTheProcessAffinityMask)
{
#if defined(_WIN32) || (defined(__linux__) && !defined(__EMSCRIPTEN__))
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "A machine with one processor has no mask that limits it.";
    const AffinityNarrowedToOneProcessor narrowed;
#if defined(_WIN32)
    if (narrowed.SpansProcessorGroups())
        GTEST_SKIP() << "The process has threads in more than one processor group, so it has no one mask to narrow.";
#endif
    ASSERT_TRUE(narrowed.IsNarrowed()) << "The read or the write of the affinity mask failed.";
    const AvailableProcessors available = AvailableProcessors::Query();
    EXPECT_EQ(available.Count, 1u);
    EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::AffinityMask);
    JobSystem::WorkStealingThreadPool pool;
    EXPECT_EQ(pool.GetWorkerCount(), 1u);
#else
    // macOS and the browser have no affinity API, so the test cannot narrow the process, and Query gives the machine.
    const AvailableProcessors available = AvailableProcessors::Query();
    EXPECT_EQ(available.Count, std::max(1u, std::thread::hardware_concurrency()));
    EXPECT_EQ(available.LimitedBy, AvailableProcessors::Limit::Machine);
#endif
}

// The default pool starts one worker per processor the process may run on.
TEST(AvailableProcessors, DefaultPoolStartsOneWorkerPerAvailableProcessor)
{
    JobSystem::WorkStealingThreadPool pool;
    EXPECT_EQ(pool.GetWorkerCount(), AvailableProcessors::Query().Count);
}
