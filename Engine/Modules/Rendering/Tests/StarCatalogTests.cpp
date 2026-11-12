// The night sky's procedural star field (Source/Sky/StarCatalog.h). Pure CPU, no device.

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Source/Sky/StarCatalog.h"
#include "Types/Color.h"
#include "Types/ColorUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

using GameEngine::ColorLinear;
using GameEngine::ColorUtils::LinearRec709Luminance;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector4;
using GameEngine::Rendering::BlackbodyLinearRec709;
using GameEngine::Rendering::BuildStarField;
using GameEngine::Rendering::kStarFieldFaintestMagnitude;
using GameEngine::Rendering::StarFieldCountBrighterThan;

namespace
{

double CieLobe(double nm, double peak, double widthBelow, double widthAbove)
{
    const double t = (nm - peak) / (nm < peak ? widthBelow : widthAbove);
    return std::exp(-0.5 * t * t);
}

// Planck's law against the CIE fit, every term evaluated at each wavelength: the reference the
// tabulated integral in BlackbodyLinearRec709 must match.
ColorLinear DirectBlackbodyLinearRec709(double kelvin)
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    for (double nm = 380.0; nm <= 780.0; nm += 2.0)
    {
        const double metres = nm * 1e-9;
        const double planck = 1.0 / (std::pow(metres, 5.0) * (std::exp(1.4387769e-2 / (metres * kelvin)) - 1.0));
        x += planck * (1.056 * CieLobe(nm, 599.8, 37.9, 31.0) + 0.362 * CieLobe(nm, 442.0, 16.0, 26.7) -
                       0.065 * CieLobe(nm, 501.1, 20.4, 26.2));
        y += planck * (0.821 * CieLobe(nm, 568.8, 46.9, 40.5) + 0.286 * CieLobe(nm, 530.9, 16.3, 31.1));
        z += planck * (1.217 * CieLobe(nm, 437.0, 11.8, 36.0) + 0.681 * CieLobe(nm, 459.0, 26.0, 13.8));
    }
    x /= y;
    z /= y;
    std::array<double, 3> rgb = {3.2406 * x - 1.5372 - 0.4986 * z, -0.9689 * x + 1.8758 + 0.0415 * z,
                                 0.0557 * x - 0.2040 + 1.0570 * z};
    for (double& channel : rgb)
        channel = std::max(channel, 0.0);
    const double luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
    return ColorLinear(static_cast<float>(rgb[0] / luminance), static_cast<float>(rgb[1] / luminance),
                       static_cast<float>(rgb[2] / luminance));
}

// A uniform value in [0, 1) from the generator's top 24 bits.
double Uniform01(std::mt19937& rng)
{
    constexpr double kTopBitsScale = 1.0 / 16777216.0;
    return static_cast<double>(rng() >> 8) * kTopBitsScale;
}

// Each star's temperature in kelvin, drawn as BuildStarField draws it: from std::mt19937 seeded with 42,
// five uniform values per star (direction z, azimuth, class pick, position in the class, twinkle seed),
// with the class picked from the naked-eye spectral mix.
std::vector<float> StarFieldTemperatures(size_t starCount)
{
    struct SpectralClass
    {
        double Share;
        float HotKelvin;
        float CoolKelvin;
    };
    constexpr std::array<SpectralClass, 6> kSpectralMix = {{
        {0.20, 25000.0f, 10000.0f},
        {0.22, 10000.0f, 7500.0f},
        {0.13, 7500.0f, 6000.0f},
        {0.14, 6000.0f, 5200.0f},
        {0.27, 5200.0f, 3900.0f},
        {0.04, 3900.0f, 2800.0f},
    }};
    std::mt19937 rng(42u);

    std::vector<float> temperatures;
    temperatures.reserve(starCount);
    for (size_t star = 0; star < starCount; ++star)
    {
        Uniform01(rng);
        Uniform01(rng);
        const double classPick = Uniform01(rng);
        const double withinClass = Uniform01(rng);
        Uniform01(rng);
        float kelvin = kSpectralMix.back().CoolKelvin;
        double cumulative = 0.0;
        for (const SpectralClass& spectral : kSpectralMix)
        {
            cumulative += spectral.Share;
            if (classPick < cumulative)
            {
                kelvin = spectral.CoolKelvin + static_cast<float>(withinClass) * (spectral.HotKelvin - spectral.CoolKelvin);
                break;
            }
        }
        temperatures.push_back(kelvin);
    }
    return temperatures;
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
    for (size_t axis = 0; axis < 3; ++axis)
    {
        size_t positive = 0;
        for (const auto& star : stars)
        {
            const Vector3 direction(star.dirAndMagnitude.x, star.dirAndMagnitude.y, star.dirAndMagnitude.z);
            ASSERT_NEAR(direction.LengthSquared(), 1.0f, 1e-5f);
            positive += direction[axis] > 0.0f ? 1u : 0u;
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
    EXPECT_GT(cool.r, cool.g);
    EXPECT_GT(cool.g, cool.b);
    EXPECT_GT(hot.b, hot.g);
    EXPECT_GT(hot.g, hot.r);
    for (const float channel : {white.r, white.g, white.b})
        EXPECT_NEAR(channel, 1.0f, 0.06f) << "a 6,504 K blackbody is near white";
    for (const ColorLinear& rgb : {cool, white, hot})
        EXPECT_NEAR(LinearRec709Luminance(rgb), 1.0f, 1e-4f);
}

// The integral reads its wavelength terms from a table built once; every colour matches the
// integral evaluated term by term over the whole spectral mix, 2,800 K to 25,000 K.
TEST(StarCatalog, TabulatedBlackbodyMatchesTheDirectIntegral)
{
    for (float kelvin = 2800.0f; kelvin <= 25000.0f; kelvin += 37.0f)
    {
        const auto tabulated = BlackbodyLinearRec709(kelvin);
        const auto direct = DirectBlackbodyLinearRec709(kelvin);
        for (float ColorLinear::*channel : {&ColorLinear::r, &ColorLinear::g, &ColorLinear::b})
            EXPECT_NEAR(tabulated.*channel, direct.*channel, 1e-6 * std::max(1.0f, direct.*channel)) << kelvin << " K";
    }
}

// The field reads star colours from a table of the blackbody integral: every star's colour is within
// 1e-4 per channel of BlackbodyLinearRec709 at the star's own temperature.
TEST(StarCatalog, EveryStarColourMatchesTheBlackbodyOfItsTemperature)
{
    constexpr float kChannelTolerance = 1e-4f;
    const auto stars = BuildStarField();
    const std::vector<float> temperatures = StarFieldTemperatures(stars.size());
    for (size_t starIndex = 0; starIndex < stars.size(); ++starIndex)
    {
        const ColorLinear exact = BlackbodyLinearRec709(temperatures[starIndex]);
        const Vector4& color = stars[starIndex].colorAndSeed;
        ASSERT_NEAR(color.x, exact.r, kChannelTolerance) << "star " << starIndex << " at " << temperatures[starIndex] << " K";
        ASSERT_NEAR(color.y, exact.g, kChannelTolerance) << "star " << starIndex << " at " << temperatures[starIndex] << " K";
        ASSERT_NEAR(color.z, exact.b, kChannelTolerance) << "star " << starIndex << " at " << temperatures[starIndex] << " K";
    }
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
        const ColorLinear color(star.colorAndSeed.x, star.colorAndSeed.y, star.colorAndSeed.z);
        ASSERT_NEAR(LinearRec709Luminance(color), 1.0f, 1e-4f);
        const float blueOverRed = color.b / std::max(color.r, 1e-6f);
        hot += blueOverRed > hotEdge.b / hotEdge.r ? 1u : 0u;
        cool += blueOverRed < coolEdge.b / coolEdge.r ? 1u : 0u;
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
