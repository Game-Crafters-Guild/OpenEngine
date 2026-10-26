#include "Mathematics/Vector3Int.h"
#include "Mathematics/VectorPrinting.h"

#include <gtest/gtest.h>

using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector3Int;

TEST(Vector3IntTests, IndexReadsTheComponentsInAConstantExpression)
{
    constexpr Vector3Int v(1, -2, 3);
    static_assert(v[0] == 1 && v[1] == -2 && v[2] == 3);
    static_assert(Vector3Int()[0] == 0 && Vector3Int()[1] == 0 && Vector3Int()[2] == 0);
    EXPECT_EQ(v[1], v.y);
}

TEST(Vector3IntTests, EqualityComparesEveryComponent)
{
    EXPECT_EQ(Vector3Int(1, 2, 3), Vector3Int(1, 2, 3));
    EXPECT_NE(Vector3Int(1, 2, 3), Vector3Int(9, 2, 3));
    EXPECT_NE(Vector3Int(1, 2, 3), Vector3Int(1, 9, 3));
    EXPECT_NE(Vector3Int(1, 2, 3), Vector3Int(1, 2, 9));
}

TEST(Vector3IntTests, AddsAndDividesComponentByComponent)
{
    EXPECT_EQ(Vector3Int(1, -1, 1) + Vector3Int(1, 1, -1), Vector3Int(2, 0, 0));
    EXPECT_EQ(Vector3Int(4, -4, 7) / 4, Vector3Int(1, -1, 1));
}

TEST(Vector3IntTests, ConvertsToTheSameFloatPoint)
{
    const Vector3 point(Vector3Int(-1, 0, 16777216));
    EXPECT_EQ(point.x, -1.0f);
    EXPECT_EQ(point.y, 0.0f);
    EXPECT_EQ(point.z, 16777216.0f);
}

TEST(Vector3IntTests, PrintsTheComponents)
{
    EXPECT_EQ(testing::PrintToString(Vector3Int(1, -2, 3)), "(1, -2, 3)");
}
