#include "Paving/PavingSeam.h"

#include "Rendering/Materials/MaterialDocument.h"

#include <gtest/gtest.h>

#include <cmath>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector2;

// A field `pitch` tiles per meter whose U runs along `degrees` from +X and whose V runs a quarter turn on.
SeamFieldFrame Field(float32 degrees, float32 pitch)
{
    const float32 radians = degrees * 3.14159265f / 180.0f;
    const Vector2 along(std::cos(radians), std::sin(radians));
    return SeamFieldFrame{along * pitch, Vector2(-along.y, along.x) * pitch};
}

TEST(PavingSeamTests, ASquarePatternAtEqualPitchesTakesAQuarterTurn)
{
    const std::optional<SeamAxisMove> move = SolveSeamAxisMove(MaterialPatternSymmetry::Square, Field(0.0f, 1.0f), Field(90.0f, 1.0f));
    ASSERT_TRUE(move.has_value());
    EXPECT_TRUE(move->Swap);
    // The other field's V falls along +X, so the move flips it to rise with the yielding field's U.
    EXPECT_EQ(move->Sign, Vector2(-1.0f, 1.0f));
}

TEST(PavingSeamTests, ARectangularPatternHasNoQuarterTurn)
{
    EXPECT_FALSE(SolveSeamAxisMove(MaterialPatternSymmetry::Rectangular, Field(0.0f, 1.0f), Field(90.0f, 1.0f)).has_value());
}

TEST(PavingSeamTests, ARectangularPatternTakesAHalfTurn)
{
    const std::optional<SeamAxisMove> move =
        SolveSeamAxisMove(MaterialPatternSymmetry::Rectangular, Field(0.0f, 1.0f), Field(185.0f, 1.0f));
    ASSERT_TRUE(move.has_value());
    EXPECT_FALSE(move->Swap);
    EXPECT_EQ(move->Sign, Vector2(-1.0f, -1.0f));
}

TEST(PavingSeamTests, UnequalPitchesTakeTheCrossfade)
{
    EXPECT_FALSE(SolveSeamAxisMove(MaterialPatternSymmetry::Square, Field(0.0f, 1.0f), Field(0.0f, 1.05f)).has_value());
    EXPECT_TRUE(SolveSeamAxisMove(MaterialPatternSymmetry::Square, Field(0.0f, 1.0f), Field(0.0f, 1.005f)).has_value());
}

TEST(PavingSeamTests, AxesFortyFiveDegreesApartTakeTheCrossfade)
{
    EXPECT_FALSE(SolveSeamAxisMove(MaterialPatternSymmetry::Square, Field(0.0f, 1.0f), Field(45.0f, 1.0f)).has_value());
    EXPECT_TRUE(SolveSeamAxisMove(MaterialPatternSymmetry::Square, Field(0.0f, 1.0f), Field(25.0f, 1.0f)).has_value());
}

TEST(PavingSeamTests, AnUntaggedPatternTakesTheCrossfade)
{
    EXPECT_FALSE(SolveSeamAxisMove(MaterialPatternSymmetry::None, Field(0.0f, 1.0f), Field(0.0f, 1.0f)).has_value());
}

} // namespace
} // namespace GameEngine::SplineRecipes
