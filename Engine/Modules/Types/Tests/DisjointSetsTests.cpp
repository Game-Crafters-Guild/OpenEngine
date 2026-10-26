#include "Types/DisjointSets.h"

#include <gtest/gtest.h>

using GameEngine::DisjointSets;

TEST(DisjointSetsTests, EachItemStartsInASetOfItsOwn)
{
    DisjointSets sets(4);
    for (GameEngine::uint32 item = 0; item < 4; ++item)
        EXPECT_EQ(sets.Find(item), item);
}

TEST(DisjointSetsTests, JoinsChainIntoOneSetAndLeaveTheRestApart)
{
    DisjointSets sets(6);
    sets.Join(0, 1);
    sets.Join(2, 3);
    sets.Join(1, 3);
    sets.Join(4, 4);
    const GameEngine::uint32 root = sets.Find(0);
    for (const GameEngine::uint32 item : {1u, 2u, 3u})
        EXPECT_EQ(sets.Find(item), root);
    EXPECT_NE(sets.Find(4), root);
    EXPECT_NE(sets.Find(5), root);
    EXPECT_NE(sets.Find(4), sets.Find(5));
}
