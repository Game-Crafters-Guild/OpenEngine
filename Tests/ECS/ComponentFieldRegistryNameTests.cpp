// ComponentFieldRegistry::FindByName: the name index follows each registration of a type, and a worker
// thread reads it while the main thread registers.

#include "ECS/Component/ComponentFieldRegistry.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using GameEngine::ECS::ComponentFieldRegistry;
using GameEngine::ECS::ComponentTypeId;
using JobSystem::TaskHandle;
using JobSystem::WorkStealingThreadPool;

namespace
{
// More names than two blocks of the index hold.
constexpr std::size_t kWorkerNames = 300;
constexpr ComponentTypeId kFirstWorkerType = 0x4e414d4557524b00ull;
// The main thread tells the worker about the names in steps of this many. Between two steps, only the
// index orders the worker's reads of the newest records.
constexpr std::size_t kNamesPerStep = 16;

// The names that the main thread registers, and what the worker reports about its lookups.
struct WorkerLookups
{
    std::vector<std::string> Names;
    std::atomic<std::size_t> Registered{0};
    std::atomic<std::size_t> WrongLookups{0};
    std::atomic<bool> Runs{false};
    std::atomic<bool> OnWorker{false};
};

// Looks up each name that the main thread reports as registered. Between those lookups, it looks up a
// name of the same length that no type has: it compares the text of every record, and must give 0.
void LookUpEachNameAsItIsRegistered(WorkerLookups& lookups, std::thread::id mainThread)
{
    lookups.OnWorker.store(std::this_thread::get_id() != mainThread);
    lookups.Runs.store(true, std::memory_order_release);
    std::size_t checked = 0;
    while (checked < kWorkerNames)
    {
        const std::size_t ready = lookups.Registered.load(std::memory_order_acquire);
        for (; checked < ready; ++checked)
        {
            if (ComponentFieldRegistry::FindByName(lookups.Names[checked]) != kFirstWorkerType + checked)
                lookups.WrongLookups.fetch_add(1);
        }
        if (ComponentFieldRegistry::FindByName("Probe.NameIndexWorker9999") != 0)
            lookups.WrongLookups.fetch_add(1);
    }
}
} // namespace

TEST(ComponentFieldRegistryNameTest, FindByNameFollowsARenameARemovalAndARegistrationAgain)
{
    constexpr ComponentTypeId kType = 0x4e414d4549445831ull;
    ComponentFieldRegistry::RegisterOwned(kType, {}, "Probe.NameIndexFirst");
    EXPECT_EQ(ComponentFieldRegistry::FindByName("NameIndexFirst"), kType);

    ComponentFieldRegistry::RegisterOwned(kType, {}, "Probe.NameIndexSecond");
    EXPECT_EQ(ComponentFieldRegistry::FindByName("NameIndexFirst"), 0u);
    EXPECT_EQ(ComponentFieldRegistry::FindByName("nameindexsecond"), kType);

    ComponentFieldRegistry::UnregisterForTests(kType);
    EXPECT_EQ(ComponentFieldRegistry::FindByName("NameIndexSecond"), 0u);

    ComponentFieldRegistry::RegisterOwned(kType, {}, "Probe.NameIndexSecond");
    EXPECT_EQ(ComponentFieldRegistry::FindByName("Probe.NameIndexSecond"), kType);
    ComponentFieldRegistry::UnregisterForTests(kType);
}

// A worker looks up names while the main thread registers more names than two blocks of the index hold.
TEST(ComponentFieldRegistryNameTest, AWorkerFindsEachNameWhileTheMainThreadRegistersPastABlock)
{
    WorkerLookups lookups;
    for (std::size_t i = 0; i < kWorkerNames; ++i)
        lookups.Names.push_back("Probe.NameIndexWorker" + std::to_string(1000 + i));

    WorkStealingThreadPool pool(2);
    ASSERT_FALSE(pool.IsInlineMode());
    const std::thread::id mainThread = std::this_thread::get_id();
    TaskHandle reader = pool.Submit([&lookups, mainThread] { LookUpEachNameAsItIsRegistered(lookups, mainThread); });
    while (!lookups.Runs.load(std::memory_order_acquire))
        std::this_thread::yield();
    for (std::size_t i = 0; i < kWorkerNames; ++i)
    {
        ComponentFieldRegistry::RegisterOwned(kFirstWorkerType + i, {}, lookups.Names[i]);
        if ((i + 1) % kNamesPerStep == 0 || i + 1 == kWorkerNames)
            lookups.Registered.store(i + 1, std::memory_order_release);
    }
    reader.Wait();

    EXPECT_TRUE(lookups.OnWorker.load());
    EXPECT_EQ(lookups.WrongLookups.load(), 0u);
    std::size_t wrongOnMain = 0;
    for (std::size_t i = 0; i < kWorkerNames; ++i)
    {
        if (ComponentFieldRegistry::FindByName(lookups.Names[i]) != kFirstWorkerType + i)
            ++wrongOnMain;
    }
    EXPECT_EQ(wrongOnMain, 0u);
    for (std::size_t i = 0; i < kWorkerNames; ++i)
        ComponentFieldRegistry::UnregisterForTests(kFirstWorkerType + i);
}
