#include "Image/Resample.h"

#include "Types/Fnv1a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace GameEngine;

namespace
{
// The pattern of the cook's mip-chain golden (TextureCookFilter.MipChainBytesMatchGolden
// in EngineTextureTests): 37x23 with varying alpha, so the odd extents reach the
// filters' edge taps and the alpha separates the color path from the data path.
std::vector<uint8_t> MakeGoldenPattern()
{
    std::vector<uint8_t> rgba(37u * 23u * 4u);
    for (uint32_t y = 0; y < 23; ++y)
        for (uint32_t x = 0; x < 37; ++x)
        {
            uint8_t* px = rgba.data() + (static_cast<size_t>(y) * 37 + x) * 4;
            px[0] = static_cast<uint8_t>(x * 37 + y * 91);
            px[1] = static_cast<uint8_t>(x * x + 3 * y);
            px[2] = static_cast<uint8_t>((x ^ y) * 11);
            px[3] = static_cast<uint8_t>(x * 7 + y * 13);
        }
    return rgba;
}

// Halves the pattern down to 1x1, as the cook builds a mip chain, and hashes every
// level's bytes in order.
uint64_t HashHalvingChain(Image::ResampleColorSpace colorSpace)
{
    std::vector<uint8_t> level = MakeGoldenPattern();
    uint32_t width = 37;
    uint32_t height = 23;
    uint64_t hash = Hashing::Fnv1a64(level.data(), level.size());
    while (width > 1 || height > 1)
    {
        const uint32_t nextWidth = std::max(width / 2u, 1u);
        const uint32_t nextHeight = std::max(height / 2u, 1u);
        std::vector<uint8_t> next(static_cast<size_t>(nextWidth) * nextHeight * 4);
        EXPECT_TRUE(Image::ResampleRgba8(level, width, height, next, nextWidth, nextHeight, colorSpace));
        hash = Hashing::Fnv1a64(next.data(), next.size(), hash);
        level = std::move(next);
        width = nextWidth;
        height = nextHeight;
    }
    return hash;
}
} // namespace

// The cook's mip chain of this pattern; TextureCookFilter.MipChainBytesMatchGolden
// pins the same hashes.
TEST(ImageResample, Rgba8SrgbMatchesTheCooksMipChain)
{
    EXPECT_EQ(HashHalvingChain(Image::ResampleColorSpace::Srgb), 0x9763e30eb4d47a5cull);
}

TEST(ImageResample, Rgba8LinearMatchesTheCooksMipChain)
{
    EXPECT_EQ(HashHalvingChain(Image::ResampleColorSpace::Linear), 0xc3ea2747e3b97022ull);
}

// The TextureCompiler's mip filter for linear input: color weighted by alpha, without
// the sRGB curve (stbir_resize_uint8_linear with STBIR_RGBA).
TEST(ImageResample, Rgba8LinearAlphaWeightedMatchesTheCompilersMipChain)
{
    EXPECT_EQ(HashHalvingChain(Image::ResampleColorSpace::LinearAlphaWeighted), 0xef7ee921c3b80afeull);
}

TEST(ImageResample, LongEdgeKeepsTheAspect)
{
    const std::vector<uint8_t> wide(400u * 100u * 4u, 128);
    const Image::Image8 landscape = Image::ResampleToLongEdge(wide, 400, 100, 64);
    ASSERT_TRUE(landscape.Valid);
    EXPECT_EQ(landscape.Width, 64u);
    EXPECT_EQ(landscape.Height, 16u);
    EXPECT_EQ(landscape.Channels, 4u);
    EXPECT_EQ(landscape.Pixels.size(), 64u * 16u * 4u);

    const Image::Image8 portrait = Image::ResampleToLongEdge(wide, 100, 400, 64);
    ASSERT_TRUE(portrait.Valid);
    EXPECT_EQ(portrait.Width, 16u);
    EXPECT_EQ(portrait.Height, 64u);

    // 23 * 32 / 37 = 19.9: the short edge rounds to the nearest pixel.
    const Image::Image8 odd = Image::ResampleToLongEdge(MakeGoldenPattern(), 37, 23, 32);
    ASSERT_TRUE(odd.Valid);
    EXPECT_EQ(odd.Width, 32u);
    EXPECT_EQ(odd.Height, 20u);
}
