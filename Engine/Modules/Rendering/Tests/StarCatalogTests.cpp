// The night sky's procedural star field (Source/Sky/StarCatalog.h). Pure CPU, no device.

#include "Source/Sky/StarCatalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using GameEngine::Rendering::BlackbodyLinearRec709;
using GameEngine::Rendering::BuildStarField;
using GameEngine::Rendering::kStarFieldFaintestMagnitude;
using GameEngine::Rendering::StarFieldCountBrighterThan;

namespace
{

float Luminance(const float* rgb)
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

} // namespace

// The field has the naked-eye sky's counts: about 9,000 stars to magnitude 6.5 and 50,600 to 8 over
// the whole sphere, ten times fewer every two magnitudes brighter (N grows as 10^(0.5 m)), sorted brightest
// first, so the instance count for a limiting magnitude draws exactly the stars at least that bright.
TEST(StarCatalog, CountsFollowTheNakedEyeSkyBrightestFirst)
{
    EXPECT_EQ(StarFieldCountBrighterThan(6.5f), 9000u);
    EXPECT_EQ(StarFieldCountBrighterThan(4.5f), 900u);
    EXPECT_EQ(kStarFieldFaintestMagnitude, 8.0f) << "the field reaches magnitude 8, the faint stars of a dark site";
    EXPECT_EQ(StarFieldCountBrighterThan(8.0f), 50611u);

    const auto stars = BuildStarField();
    ASSERT_EQ(stars.size(), StarFieldCountBrighterThan(kStarFieldFaintestMagnitude));
    EXPECT_FLOAT_EQ(stars.front().dirAndMagnitude[3], -1.46f) << "the brightest star is Sirius";
    for (size_t i = 1; i < stars.size(); ++i)
        ASSERT_LE(stars[i - 1].dirAndMagnitude[3], stars[i].dirAndMagnitude[3]) << "star " << i << " is out of order";

    for (const float limit : {1.0f, 3.0f, 5.0f, 6.5f})
    {
        size_t brighter = 0;
        for (const auto& star : stars)
            brighter += star.dirAndMagnitude[3] <= limit ? 1u : 0u;
        EXPECT_NEAR(static_cast<double>(brighter), static_cast<double>(StarFieldCountBrighterThan(limit)), 1.0)
            << "stars at least as bright as magnitude " << limit;
    }
}

// Directions are unit vectors spread evenly over the sphere: each hemisphere of the three axes
// holds half the field, within 2 %.
TEST(StarCatalog, DirectionsCoverTheSphereEvenly)
{
    const auto stars = BuildStarField();
    for (int axis = 0; axis < 3; ++axis)
    {
        size_t positive = 0;
        for (const auto& star : stars)
        {
            const float* d = star.dirAndMagnitude;
            ASSERT_NEAR(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-5f);
            positive += d[axis] > 0.0f ? 1u : 0u;
        }
        EXPECT_NEAR(static_cast<double>(positive) / static_cast<double>(stars.size()), 0.5, 0.02) << "axis " << axis;
    }
}

// A blackbody's colour: a cool star is red over green over blue, a hot one blue over green over
// red, and one near 6,500 K is close to D65's white. Each has luminance 1.
TEST(StarCatalog, BlackbodyColoursRunFromRedToBlue)
{
    const auto cool = BlackbodyLinearRec709(3000.0f);
    const auto white = BlackbodyLinearRec709(6504.0f);
    const auto hot = BlackbodyLinearRec709(20000.0f);
    EXPECT_GT(cool[0], cool[1]);
    EXPECT_GT(cool[1], cool[2]);
    EXPECT_GT(hot[2], hot[1]);
    EXPECT_GT(hot[1], hot[0]);
    for (const float channel : white)
        EXPECT_NEAR(channel, 1.0f, 0.06f) << "a 6,504 K blackbody is near white";
    for (const auto& rgb : {cool, white, hot})
        EXPECT_NEAR(Luminance(rgb.data()), 1.0f, 1e-4f);
}

// The spectral mix of naked-eye stars: 42 % are B or A (hotter than 7,500 K, bluer than that
// edge) and 31 % are K or M (cooler than 5,200 K, redder). Every colour has luminance 1, so the
// colour never changes a star's brightness.
TEST(StarCatalog, ColoursFollowTheNakedEyeSpectralMix)
{
    const auto hotEdge = BlackbodyLinearRec709(7500.0f);
    const auto coolEdge = BlackbodyLinearRec709(5200.0f);
    const auto stars = BuildStarField();
    size_t hot = 0;
    size_t cool = 0;
    for (const auto& star : stars)
    {
        const float* rgb = star.colorAndSeed;
        ASSERT_NEAR(Luminance(rgb), 1.0f, 1e-4f);
        const float blueOverRed = rgb[2] / std::max(rgb[0], 1e-6f);
        hot += blueOverRed > hotEdge[2] / hotEdge[0] ? 1u : 0u;
        cool += blueOverRed < coolEdge[2] / coolEdge[0] ? 1u : 0u;
    }
    const double count = static_cast<double>(stars.size());
    EXPECT_NEAR(static_cast<double>(hot) / count, 0.42, 0.02);
    EXPECT_NEAR(static_cast<double>(cool) / count, 0.31, 0.02);
}

// The field is seeded and built from std::mt19937's raw output, which the standard fixes, so every
// run on every platform draws the same sky. The golden values were computed from the generator's
// specified sequence outside the engine (a Python replica of mt19937 and of BuildStarField), so a
// platform whose build drifts from it fails here.
TEST(StarCatalog, IsTheSameSkyEveryRunAndOnEveryPlatform)
{
    const auto stars = BuildStarField();
    const auto again = BuildStarField();
    ASSERT_EQ(stars.size(), again.size());
    for (size_t i = 0; i < stars.size(); i += 997)
        for (int k = 0; k < 4; ++k)
        {
            EXPECT_EQ(stars[i].dirAndMagnitude[k], again[i].dirAndMagnitude[k]);
            EXPECT_EQ(stars[i].colorAndSeed[k], again[i].colorAndSeed[k]);
        }

    ASSERT_GT(stars.size(), 1000u);
    EXPECT_NEAR(stars[0].dirAndMagnitude[0], 0.2790646f, 1e-5f);
    EXPECT_NEAR(stars[0].dirAndMagnitude[1], -0.2509198f, 1e-5f);
    EXPECT_NEAR(stars[0].dirAndMagnitude[2], -0.9269100f, 1e-5f);
    EXPECT_FLOAT_EQ(stars[0].dirAndMagnitude[3], -1.46f) << "Sirius";
    EXPECT_NEAR(stars[1000].dirAndMagnitude[3], 4.591949f, 1e-4f);
    EXPECT_NEAR(stars[1000].colorAndSeed[0], 1.263765f, 1e-3f);
    EXPECT_NEAR(stars[1000].colorAndSeed[1], 0.952898f, 1e-3f);
    EXPECT_NEAR(stars[1000].colorAndSeed[2], 0.689898f, 1e-3f);
}
