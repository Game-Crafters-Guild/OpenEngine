#include "Assets/SvgRasterizer.h"
#include "Assets/TextureAsset.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

namespace GameEngine
{
namespace
{

TEST(SvgRasterizerTests, ParsesAndClampsPerAssetRasterSize)
{
    uint32 size = 256;
    EXPECT_TRUE(ParseSvgRasterSizeMeta("1024", size));
    EXPECT_EQ(size, 1024u);

    EXPECT_TRUE(ParseSvgRasterSizeMeta("1", size));
    EXPECT_EQ(size, kMinSvgRasterSize);

    EXPECT_TRUE(ParseSvgRasterSizeMeta("999999", size));
    EXPECT_EQ(size, kMaxSvgRasterSize);
}

TEST(SvgRasterizerTests, RejectsMalformedPerAssetRasterSize)
{
    uint32 size = 512;
    EXPECT_FALSE(ParseSvgRasterSizeMeta("", size));
    EXPECT_FALSE(ParseSvgRasterSizeMeta("1024px", size));
    EXPECT_FALSE(ParseSvgRasterSizeMeta("-32", size));
    EXPECT_EQ(size, 512u);
}

TEST(SvgRasterizerTests, CalculatesAspectPreservingRasterDimensions)
{
    uint32 width = 0;
    uint32 height = 0;

    EXPECT_TRUE(CalculateSvgRasterDimensions(20.0f, 10.0f, 80.0f, width, height));
    EXPECT_EQ(width, 80u);
    EXPECT_EQ(height, 40u);

    EXPECT_TRUE(CalculateSvgRasterDimensions(10.0f, 20.0f, 80.0f, width, height));
    EXPECT_EQ(width, 40u);
    EXPECT_EQ(height, 80u);

    EXPECT_TRUE(CalculateSvgRasterDimensions(1000.0f, 333.0f, 512.0f, width, height));
    EXPECT_EQ(width, 512u);
    EXPECT_EQ(height, 170u);

    EXPECT_FALSE(CalculateSvgRasterDimensions(0.0f, 10.0f, 80.0f, width, height));
}

TEST(SvgRasterizerTests, RasterizesToExplicitLongestAxisSize)
{
    constexpr const char* svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">)"
        R"(<rect width="20" height="10" fill="#fff"/></svg>)";

    float32 sourceWidth = 0.0f;
    float32 sourceHeight = 0.0f;
    if (!GetSvgSourceSize(svg, sourceWidth, sourceHeight))
        GTEST_SKIP() << "ThorVG is not available in this build";
    EXPECT_FLOAT_EQ(sourceWidth, 20.0f);
    EXPECT_FLOAT_EQ(sourceHeight, 10.0f);

    SvgRasterizedImage image;
    ASSERT_TRUE(RasterizeSvgToRgbaAtSize(svg, 80.0f, image));

    EXPECT_EQ(image.Width, 80u);
    EXPECT_EQ(image.Height, 40u);
    EXPECT_EQ(image.DataSize, 80u * 40u * 4u);
    EXPECT_NE(image.Data.get(), nullptr);
}

// A UI draws one SVG at many on-screen sizes, so the loaded asset keeps its
// source and rasterizes it again at any requested size.
TEST(SvgRasterizerTests, TextureAssetRasterizesItsSvgSourceAtEachRequestedSize)
{
    constexpr const char* svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10" viewBox="0 0 20 10">)"
        R"(<rect width="20" height="10" fill="#fff"/></svg>)";
    float32 probeWidth = 0.0f;
    float32 probeHeight = 0.0f;
    if (!GetSvgSourceSize(svg, probeWidth, probeHeight))
        GTEST_SKIP() << "ThorVG is not available in this build";

    TextureAsset asset(GUID::Generate(), "icon.svg");
    const std::string text(svg);
    ASSERT_TRUE(asset.LoadFromData(Vector<uint8>(text.begin(), text.end())));
    const std::shared_ptr<const TextureAsset::SvgSource> source = asset.SnapshotSvgSource();
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->Text, text);
    EXPECT_FLOAT_EQ(source->Width, 20.0f);
    EXPECT_FLOAT_EQ(source->Height, 10.0f);

    SvgRasterizedImage small;
    ASSERT_TRUE(RasterizeSvgToRgbaAtDimensions(source->Text, 32, 16, small));
    EXPECT_EQ(small.Width, 32u);
    EXPECT_EQ(small.Height, 16u);

    SvgRasterizedImage large;
    ASSERT_TRUE(RasterizeSvgToRgbaAtDimensions(source->Text, 80, 40, large));
    EXPECT_EQ(large.Width, 80u);
    EXPECT_EQ(large.Height, 40u);

    // A box of another aspect gets a raster of exactly that box.
    SvgRasterizedImage square;
    ASSERT_TRUE(RasterizeSvgToRgbaAtDimensions(source->Text, 15, 15, square));
    EXPECT_EQ(square.Width, 15u);
    EXPECT_EQ(square.Height, 15u);
    EXPECT_EQ(square.DataSize, 15u * 15u * 4u);

    SvgRasterizedImage empty;
    EXPECT_FALSE(RasterizeSvgToRgbaAtDimensions(source->Text, 0, 15, empty));

    // Unloading drops the asset's source; a snapshot taken before stays whole.
    asset.Unload();
    EXPECT_EQ(asset.SnapshotSvgSource(), nullptr);
    EXPECT_EQ(source->Text, text);
}

// A drawn-size raster holds the picture's coverage of each pixel. A "#" of
// four crossing strokes in one path: the rasterizer alone adds the coverage of
// two strokes where they cross (and errs on single edges), so the raster is
// checked against the same picture made 16 times larger and averaged down.
TEST(SvgRasterizerTests, DimensionRasterHoldsTheCoverageOfCrossingStrokes)
{
    constexpr const char* svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32" fill="none">)"
        R"(<path d="M4 20.175H28M4 11.8265H28M11.824 4L11.824 28M20.1725 4L20.1724 28" stroke="white")"
        R"( stroke-width="2" stroke-linecap="round"/></svg>)";
    float32 probeWidth = 0.0f;
    float32 probeHeight = 0.0f;
    if (!GetSvgSourceSize(svg, probeWidth, probeHeight))
        GTEST_SKIP() << "ThorVG is not available in this build";

    constexpr uint32 kSize = 16;
    constexpr uint32 kScale = 16;
    SvgRasterizedImage raster;
    ASSERT_TRUE(RasterizeSvgToRgbaAtDimensions(svg, kSize, kSize, raster));
    SvgRasterizedImage fine;
    ASSERT_TRUE(RasterizeSvgToRgbaAtDimensions(svg, kSize * kScale, kSize * kScale, fine));

    int worst = 0;
    for (uint32 y = 0; y < kSize; ++y)
        for (uint32 x = 0; x < kSize; ++x)
        {
            uint32 alpha = 0;
            for (uint32 sy = 0; sy < kScale; ++sy)
                for (uint32 sx = 0; sx < kScale; ++sx)
                    alpha += fine.Data[((size_t(y) * kScale + sy) * fine.Width + x * kScale + sx) * 4 + 3];
            const int reference = int((alpha + kScale * kScale / 2) / (kScale * kScale));
            worst = std::max(worst, std::abs(int(raster.Data[(size_t(y) * kSize + x) * 4 + 3]) - reference));
        }
    EXPECT_LE(worst, 8) << "alpha levels off the averaged reference";
}

} // namespace
} // namespace GameEngine
