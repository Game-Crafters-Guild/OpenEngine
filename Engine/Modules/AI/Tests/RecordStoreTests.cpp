#include "AI/RecordStore.h"

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

AI::RecordStore::Sighting At(uint32 subject, float32 x)
{
    AI::RecordStore::Sighting sighting;
    sighting.Subject = subject;
    sighting.Position = Mathematics::Vector3(x, 0.0f, 0.0f);
    return sighting;
}

} // namespace

TEST(RecordStoreTests, ObserveUpdatesTheSubjectAndKeepsFirstSeen)
{
    AI::RecordStore store;
    store.Observe(At(7, 1.0f), 3);
    store.Observe(At(7, 5.0f), 9);

    ASSERT_EQ(store.GetCount(), 1u);
    const uint32 index = store.Find(7);
    ASSERT_NE(index, AI::RecordStore::kNoRecord);
    EXPECT_EQ(store.GetFirstSeen()[index], 3u);
    EXPECT_EQ(store.GetLastSeen()[index], 9u);
    EXPECT_FLOAT_EQ(store.GetPositions()[index].x, 5.0f);
}

// At capacity 2, subject 3 replaces the record seen longest ago; between two records seen on one
// tick, the lower subject id goes.
TEST(RecordStoreTests, FullStoreReplacesTheOldestRecord)
{
    AI::RecordStore store;
    store.Capacity = 2;
    store.Observe(At(1, 0.0f), 5);
    store.Observe(At(2, 0.0f), 4);
    store.Observe(At(3, 0.0f), 6);
    EXPECT_EQ(store.GetCount(), 2u);
    EXPECT_EQ(store.Find(2), AI::RecordStore::kNoRecord);


    AI::RecordStore tied;
    tied.Capacity = 2;
    tied.Observe(At(9, 0.0f), 1);
    tied.Observe(At(8, 0.0f), 1);
    tied.Observe(At(7, 0.0f), 2);
    EXPECT_EQ(tied.Find(8), AI::RecordStore::kNoRecord);
    EXPECT_NE(tied.Find(9), AI::RecordStore::kNoRecord);
    EXPECT_NE(tied.Find(7), AI::RecordStore::kNoRecord);
}

TEST(RecordStoreTests, ForgetRemovesRecordsOlderThanTheForgetAge)
{
    AI::RecordStore store;
    store.ForgetTicks = 10;
    store.Observe(At(1, 0.0f), 0);
    store.Observe(At(2, 0.0f), 5);
    store.Forget(10);
    EXPECT_EQ(store.GetCount(), 2u);
    store.Forget(11);
    EXPECT_EQ(store.Find(1), AI::RecordStore::kNoRecord);
    EXPECT_NE(store.Find(2), AI::RecordStore::kNoRecord);
}

// The sensor sees 10 m around the origin on tick 8 and renews subject 1 only: subject 2 inside the
// area goes, subject 3 outside it stays. With ClearOnSight off, nothing goes.
TEST(RecordStoreTests, ClearSeenAreaRemovesUnrenewedRecordsInTheArea)
{
    AI::RecordStore store;
    store.Observe(At(1, 2.0f), 4);
    store.Observe(At(2, 5.0f), 4);
    store.Observe(At(3, 20.0f), 4);
    store.Observe(At(1, 3.0f), 8);
    store.ClearSeenArea(Mathematics::Vector3(0.0f, 0.0f, 0.0f), 10.0f, 8);

    EXPECT_EQ(store.GetCount(), 2u);
    EXPECT_NE(store.Find(1), AI::RecordStore::kNoRecord);
    EXPECT_EQ(store.Find(2), AI::RecordStore::kNoRecord);
    EXPECT_NE(store.Find(3), AI::RecordStore::kNoRecord);

    store.ClearOnSight = false;
    store.ClearSeenArea(Mathematics::Vector3(20.0f, 0.0f, 0.0f), 10.0f, 9);
    EXPECT_EQ(store.GetCount(), 2u);
}
