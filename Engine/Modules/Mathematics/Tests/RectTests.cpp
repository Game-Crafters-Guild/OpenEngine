#include "Mathematics/Rect.h"

#include <gtest/gtest.h>

#include <limits>

using GameEngine::Mathematics::Rect;

TEST(RectTests, IntersectionIsTheAreaBothRectsCover)
{
    const Rect a{10.0f, 20.0f, 30.0f, 40.0f};
    const Rect b{25.0f, 5.0f, 100.0f, 30.0f};
    EXPECT_EQ(a.Intersection(b), (Rect{25.0f, 20.0f, 15.0f, 15.0f}));
    EXPECT_EQ(b.Intersection(a), a.Intersection(b));
    EXPECT_EQ(a.Intersection(a), a);
}

TEST(RectTests, IntersectionOfApartOrTouchingRectsIsEmpty)
{
    const Rect a{0.0f, 0.0f, 10.0f, 10.0f};
    EXPECT_TRUE(a.Intersection(Rect{20.0f, 0.0f, 5.0f, 5.0f}).IsEmpty());
    EXPECT_TRUE(a.Intersection(Rect{10.0f, 0.0f, 5.0f, 5.0f}).IsEmpty());
    EXPECT_FALSE(a.Intersection(Rect{9.5f, 9.5f, 5.0f, 5.0f}).IsEmpty());
}

TEST(RectTests, IsEmptyWhenAWidthOrHeightIsNotPositive)
{
    EXPECT_TRUE(Rect{}.IsEmpty());
    EXPECT_TRUE((Rect{0.0f, 0.0f, 5.0f, 0.0f}).IsEmpty());
    EXPECT_TRUE((Rect{0.0f, 0.0f, -1.0f, 5.0f}).IsEmpty());
    EXPECT_TRUE((Rect{0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 5.0f}).IsEmpty());
    EXPECT_FALSE((Rect{0.0f, 0.0f, 0.5f, 0.5f}).IsEmpty());
}
