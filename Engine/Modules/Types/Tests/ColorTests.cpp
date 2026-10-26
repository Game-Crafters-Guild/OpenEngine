#include "Types/Color.h"
#include "Types/ColorUtils.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace
{

using GameEngine::ColorHSV;
using GameEngine::ColorLinear;
using GameEngine::ColorSRGB;

TEST(ColorTests, LinearColorsCompareEqualOnlyWhenEveryChannelMatches)
{
    const ColorLinear color(0.1f, 0.2f, 0.3f, 0.4f);

    EXPECT_TRUE(color == ColorLinear(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_FALSE(color != ColorLinear(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.9f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.1f, 0.9f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.1f, 0.2f, 0.9f, 0.4f));
    EXPECT_TRUE(color != ColorLinear(0.1f, 0.2f, 0.3f, 0.9f));
}

// The BT.709 weights, one channel at a time, and alpha ignored. Each value is a constant expression.
TEST(ColorTests, LinearRec709LuminanceWeighsEachChannelByItsBt709Weight)
{
    using GameEngine::ColorUtils::LinearRec709Luminance;
    constexpr float kRed = LinearRec709Luminance(ColorLinear(1.0f, 0.0f, 0.0f));
    constexpr float kGreen = LinearRec709Luminance(ColorLinear(0.0f, 1.0f, 0.0f));
    constexpr float kBlue = LinearRec709Luminance(ColorLinear(0.0f, 0.0f, 1.0f));
    constexpr float kAlphaOnly = LinearRec709Luminance(ColorLinear(0.0f, 0.0f, 0.0f, 1.0f));

    EXPECT_EQ(kRed, 0.2126f);
    EXPECT_EQ(kGreen, 0.7152f);
    EXPECT_EQ(kBlue, 0.0722f);
    EXPECT_EQ(kAlphaOnly, 0.0f);
    EXPECT_FLOAT_EQ(LinearRec709Luminance(ColorLinear(0.5f, 0.25f, 2.0f, 0.0f)), 0.1063f + 0.1788f + 0.1444f);
}

TEST(ColorTests, SrgbColorsCompareEqualOnlyWhenEveryChannelMatches)
{
    const ColorSRGB color(0.1f, 0.2f, 0.3f, 0.4f);

    EXPECT_TRUE(color == ColorSRGB(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_FALSE(color != ColorSRGB(0.1f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.9f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.1f, 0.9f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.1f, 0.2f, 0.9f, 0.4f));
    EXPECT_TRUE(color != ColorSRGB(0.1f, 0.2f, 0.3f, 0.9f));
}

TEST(ColorTests, HsvColorsCompareEqualOnlyWhenEveryChannelMatches)
{
    const ColorHSV color(120.0f, 0.2f, 0.3f, 0.4f);

    EXPECT_TRUE(color == ColorHSV(120.0f, 0.2f, 0.3f, 0.4f));
    EXPECT_FALSE(color != ColorHSV(120.0f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(240.0f, 0.2f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(120.0f, 0.9f, 0.3f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(120.0f, 0.2f, 0.9f, 0.4f));
    EXPECT_TRUE(color != ColorHSV(120.0f, 0.2f, 0.3f, 0.9f));
}

// The bytes are sRGB: mid gray 0x80 decodes to 0.2158 linear, not to 128 / 255. Black and white are fixed points.
TEST(ColorTests, ArgbToLinearDecodesSrgbBytes)
{
    using GameEngine::ColorUtils::ArgbToLinear;
    constexpr float kTolerance = 1e-4f;
    const ColorLinear gray = ArgbToLinear(0x80808080u);
    const ColorLinear white = ArgbToLinear(0xFFFFFFFFu);
    const ColorLinear black = ArgbToLinear(0xFF000000u);

    EXPECT_NEAR(gray.r, 0.2158f, kTolerance);
    EXPECT_NEAR(gray.g, 0.2158f, kTolerance);
    EXPECT_NEAR(gray.b, 0.2158f, kTolerance);
    EXPECT_NEAR(gray.a, 128.0f / 255.0f, kTolerance) << "alpha is linear coverage, not decoded";
    EXPECT_EQ(white, ColorLinear(1.0f, 1.0f, 1.0f, 1.0f));
    EXPECT_EQ(black, ColorLinear(0.0f, 0.0f, 0.0f, 1.0f));
}

// Every byte of every channel survives a decode and an encode; each channel carries a different byte, so a swap
// of two channels fails. Out-of-range linear values clamp.
TEST(ColorTests, LinearToArgbInvertsArgbToLinearForEveryByteAndClamps)
{
    using GameEngine::ColorUtils::ArgbToLinear;
    using GameEngine::ColorUtils::LinearToArgb;
    for (std::uint32_t value = 0; value < 256; ++value)
    {
        const std::uint32_t argb = 0xFF000000u | (value << 16) | (((value + 85u) & 0xFFu) << 8) | ((value + 170u) & 0xFFu);
        EXPECT_EQ(LinearToArgb(ArgbToLinear(argb)), argb) << "byte " << value;
    }
    EXPECT_EQ(LinearToArgb(ColorLinear(2.0f, -1.0f, 1.0f, 0.0f)), 0xFFFF00FFu);
}

} // namespace
