#include "Types/FlatSet.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

using GameEngine::FlatSet;

TEST(FlatSetTests, InsertAddsAKeyOnce)
{
    FlatSet<int> set;
    EXPECT_TRUE(set.Empty());
    EXPECT_TRUE(set.Insert(3));
    EXPECT_TRUE(set.Insert(1));
    EXPECT_FALSE(set.Insert(3)) << "a key already present is not added again";
    EXPECT_EQ(set.Size(), 2u);
    EXPECT_TRUE(set.Contains(1));
    EXPECT_TRUE(set.Contains(3));
    EXPECT_FALSE(set.Contains(2));
}

TEST(FlatSetTests, IterationVisitsTheKeysInOrder)
{
    FlatSet<int> set;
    for (int key : {5, 2, 9, 2, 7})
        set.Insert(key);
    EXPECT_EQ(std::vector<int>(set.begin(), set.end()), (std::vector<int>{2, 5, 7, 9}));

    FlatSet<int, std::greater<int>> descending;
    for (int key : {5, 2, 9})
        descending.Insert(key);
    EXPECT_EQ(std::vector<int>(descending.begin(), descending.end()), (std::vector<int>{9, 5, 2}));
}

TEST(FlatSetTests, EraseRemovesOnlyAPresentKey)
{
    FlatSet<int> set;
    set.Insert(1);
    set.Insert(2);
    EXPECT_TRUE(set.Erase(1));
    EXPECT_FALSE(set.Erase(1));
    EXPECT_FALSE(set.Contains(1));
    EXPECT_TRUE(set.Contains(2));
    set.Clear();
    EXPECT_TRUE(set.Empty());
}
