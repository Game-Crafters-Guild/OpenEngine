// The grass palette and presets: the automatic root and the default palette in linear light (the
// surface's decode is ColorSRGB's, GrassColorDecodeTests.cpp), and the presets
// (TerrainGrass/TerrainGrassPresets.h): Meadow is the component's defaults, a preset writes exactly
// its own dials, and an edit away from the picked preset reads Custom.

#include <gtest/gtest.h>

#include "TerrainGrass/TerrainGrassPresets.h"

#include "Types/Color.h"

#include <algorithm>

namespace GameEngine::TerrainGrass
{
namespace
{
ColorLinear DecodeArgb(uint32 argb)
{
    return ColorSRGB::FromSRGB255(static_cast<uint8>((argb >> 16) & 0xFFu), static_cast<uint8>((argb >> 8) & 0xFFu),
                                  static_cast<uint8>(argb & 0xFFu))
        .ToLinear();
}

float32 Luminance(const ColorLinear& c)
{
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

} // namespace

// Auto Root Color is the tip in shade: kTerrainGrassAutoRootLuminance of the tip's luminance in
// linear light, at the tip's chromaticity, within the 8-bit encode's rounding.
TEST(TerrainGrassPalette, AnAutomaticRootKeepsTheTipsChromaticityAtTheRootLuminanceInLinearLight)
{
    for (const uint32 tip : {0xFF79954Du, 0xFFB3DB4Du, 0xFF9CD334u, 0xFF6C8E4Bu, 0xFFA29B61u, 0xFFFFFFFFu})
    {
        const ColorLinear tipLinear = DecodeArgb(tip);
        const ColorLinear rootLinear = DecodeArgb(Components::TerrainGrassAutoRootColor(tip));
        EXPECT_NEAR(Luminance(rootLinear) / Luminance(tipLinear), Components::kTerrainGrassAutoRootLuminance, 0.02f)
            << std::hex << "tip 0x" << tip;
        const float32 tipSum = tipLinear.r + tipLinear.g + tipLinear.b;
        const float32 rootSum = rootLinear.r + rootLinear.g + rootLinear.b;
        EXPECT_NEAR(rootLinear.g / rootSum, tipLinear.g / tipSum, 0.02f) << std::hex << "tip 0x" << tip;
        EXPECT_NEAR(rootLinear.b / rootSum, tipLinear.b / tipSum, 0.02f) << std::hex << "tip 0x" << tip;
    }
}

// The default palette: a leaf tip and a brighter yellow-green back-light, in linear light.
TEST(TerrainGrassPalette, TheDefaultPaletteIsALeafAndAYellowGreenBacklight)
{
    const Components::TerrainGrass defaults{};
    const ColorLinear tip = DecodeArgb(defaults.TipColor);
    const ColorLinear backlight = DecodeArgb(defaults.BacklightColor);
    constexpr float32 kByte = 1.0f / 255.0f;
    EXPECT_NEAR(tip.r, 0.19f, kByte);
    EXPECT_NEAR(tip.g, 0.30f, kByte);
    EXPECT_NEAR(tip.b, 0.075f, kByte);
    EXPECT_NEAR(backlight.r, 0.50f, 2.0f * kByte);
    EXPECT_NEAR(backlight.g, 0.62f, 2.0f * kByte);
    EXPECT_NEAR(backlight.b, 0.12f, 2.0f * kByte);
}

TEST(TerrainGrassPresets, MeadowIsTheComponentDefaults)
{
    const Components::TerrainGrass defaults{};
    EXPECT_EQ(defaults.Preset, Components::TerrainGrassPreset::Meadow);
    Components::TerrainGrass picked{};
    ApplyTerrainGrassPreset(picked, Components::TerrainGrassPreset::Meadow);
    EXPECT_EQ(picked.TipColor, defaults.TipColor);
    EXPECT_EQ(picked.RootColor, defaults.RootColor);
    EXPECT_EQ(picked.BacklightColor, defaults.BacklightColor);
    EXPECT_FLOAT_EQ(picked.BladesPerSquareMeter, defaults.BladesPerSquareMeter);
    EXPECT_FLOAT_EQ(picked.BladeHeight, defaults.BladeHeight);
    EXPECT_FLOAT_EQ(picked.BladeWidth, defaults.BladeWidth);
    EXPECT_FLOAT_EQ(picked.BladeTilt, defaults.BladeTilt);
}

TEST(TerrainGrassPresets, APresetWritesItsDialsAndAnEditAwayFromItReadsCustom)
{
    for (const TerrainGrassPresetValues& values : kTerrainGrassPresets)
    {
        Components::TerrainGrass grass{};
        grass.WindStrength = 0.4f;
        grass.CanopyShade = 0.5f;
        ApplyTerrainGrassPreset(grass, values.Preset);
        EXPECT_EQ(grass.Preset, values.Preset);
        EXPECT_EQ(grass.TipColor, values.TipColor);
        EXPECT_FLOAT_EQ(grass.BladesPerSquareMeter, values.BladesPerSquareMeter);
        EXPECT_FLOAT_EQ(grass.BladeTilt, values.BladeTilt);
        EXPECT_FLOAT_EQ(grass.WindStrength, 0.4f) << "a preset wrote a dial it does not own";
        EXPECT_FLOAT_EQ(grass.CanopyShade, 0.5f) << "a preset wrote a dial it does not own";

        ReconcileTerrainGrassPreset(grass);
        EXPECT_EQ(grass.Preset, values.Preset) << "an unedited preset reads Custom";
        grass.BladeWidth += 0.004f;
        ReconcileTerrainGrassPreset(grass);
        EXPECT_EQ(grass.Preset, Components::TerrainGrassPreset::Custom);
    }
    // Custom names the dials as authored and writes none of them.
    Components::TerrainGrass grass{};
    grass.TipColor = 0xFF123456u;
    ApplyTerrainGrassPreset(grass, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(grass.Preset, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(grass.TipColor, 0xFF123456u);
}

} // namespace GameEngine::TerrainGrass
