#include <gtest/gtest.h>

#include "ECSModules/Rendering/Systems/KeyedWorldSubmissions.h"

#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

namespace
{

WorldSubmissionRecord MakeRecord(uint32 instance, uint32 view)
{
    WorldSubmissionRecord record{};
    record.instanceIndex = instance;
    record.viewId = Rendering::ViewId{view};
    record.renderLayerMask = GameEngine::Rendering::RenderLayerMask{instance * 7u + view};
    return record;
}

using Key = std::tuple<uint32, uint64, uint32>;

// The records' identities in the order the set holds them.
std::vector<Key> InOrder(std::span<const WorldSubmissionRecord> records)
{
    std::vector<Key> keys;
    for (const auto& record : records)
        keys.emplace_back(record.instanceIndex, static_cast<uint64>(record.viewId), record.renderLayerMask.Bits);
    return keys;
}

} // namespace

// Erasing instances removes exactly their records, whatever positions they held,
// and keeps every other record in the order it was appended: a random sequence of
// appends and batched erases matches a plain ordered reference at every step.
TEST(KeyedWorldSubmissions, RandomAppendsAndErasesKeepTheSurvivorsOrder)
{
    KeyedWorldSubmissions set;
    std::vector<WorldSubmissionRecord> reference;
    std::mt19937 rng(1234u);
    std::uniform_int_distribution<uint32> instanceOf(0u, 63u);
    std::uniform_int_distribution<uint32> viewsOf(1u, 4u);
    std::uniform_int_distribution<int> opOf(0, 2);

    for (int step = 0; step < 4000; ++step)
    {
        if (opOf(rng) == 0)
        {
            const std::vector<uint32> erased = {instanceOf(rng), instanceOf(rng), instanceOf(rng)};
            set.Erase(erased);
            std::erase_if(reference, [&](const WorldSubmissionRecord& record)
                          { return std::find(erased.begin(), erased.end(), record.instanceIndex) != erased.end(); });
        }
        else
        {
            const uint32 instance = instanceOf(rng);
            const uint32 views = viewsOf(rng);
            for (uint32 view = 0; view < views; ++view)
            {
                set.Append(MakeRecord(instance, view));
                reference.push_back(MakeRecord(instance, view));
            }
        }
        ASSERT_EQ(InOrder(set.Records()), InOrder(reference)) << "diverged at step " << step;
    }
}

TEST(KeyedWorldSubmissions, ClearForgetsEveryInstance)
{
    KeyedWorldSubmissions set;
    set.Append(MakeRecord(3, 0));
    set.Append(MakeRecord(3, 1));
    set.Append(MakeRecord(9, 0));
    set.Clear();
    EXPECT_EQ(set.Size(), 0u);
    const uint32 three[] = {3u};
    set.Erase(three);
    set.Append(MakeRecord(9, 2));
    ASSERT_EQ(set.Size(), 1u);
    EXPECT_EQ(set.Records()[0].instanceIndex, 9u);
}
