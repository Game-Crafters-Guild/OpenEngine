// A ground point read against a road's strip mapping where the lid continues the road's grid.
#include "Paving/PavingRoadUV.h"

#include <gtest/gtest.h>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector2;

TEST(PavingRoadUVTests, APointReadAgainstACenterlineWithNoLengthIsOutsideTheRoad)
{
    PavingRoadUV road;
    road.Points = {Vector2(2.0f, 3.0f), Vector2(2.0f, 3.0f)};
    road.Distance = {0.0f, 0.0f};
    road.HalfLeft = {1.0f, 1.0f};
    road.HalfRight = {1.0f, 1.0f};
    road.Spans = {Vector2(0.0f, 0.0f)};

    const PavingRoadPoint point = LocateOnRoad(road, Vector2(2.0f, 3.0f));

    EXPECT_LT(point.Inside, 0.0f);
}

} // namespace
} // namespace GameEngine::SplineRecipes
