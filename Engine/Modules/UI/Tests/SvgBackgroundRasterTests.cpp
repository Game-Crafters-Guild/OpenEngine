#include "BackgroundImageGeometry.h"
#include "SvgBackgroundRaster.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::UI;

namespace
{

BackgroundImageStyle ExplicitSize(float w, float h, bool wPercent = false, bool hPercent = false)
{
    BackgroundImageStyle bg;
    bg.SizeMode = BackgroundSizeMode::Explicit;
    bg.SizeX = w;
    bg.SizeXIsPercent = wPercent;
    bg.SizeY = h;
    bg.SizeYIsPercent = hPercent;
    return bg;
}

BackgroundImageStyle Mode(BackgroundSizeMode mode)
{
    BackgroundImageStyle bg;
    bg.SizeMode = mode;
    return bg;
}

} // namespace

// An SVG bakes at its drawn box rounded up to whole pixels on each axis: no
// ladder, no 16 px floor, and the box's own aspect.
TEST(SvgBackgroundRaster, RasterSizeIsTheDrawnBoxInWholePixels)
{
    EXPECT_EQ(SvgBackgroundRasterSize({15.0f, 15.0f}), (SvgRasterSize{15, 15}));
    EXPECT_EQ(SvgBackgroundRasterSize({30.0f, 30.0f}), (SvgRasterSize{30, 30}));
    EXPECT_EQ(SvgBackgroundRasterSize({8.0f, 8.0f}), (SvgRasterSize{8, 8})) << "below the old 16 px floor";
    EXPECT_EQ(SvgBackgroundRasterSize({16.0f, 12.0f}), (SvgRasterSize{16, 12})) << "the box's aspect, not the SVG's";
    EXPECT_EQ(SvgBackgroundRasterSize({15.68f, 15.68f}), (SvgRasterSize{16, 16})) << "a fraction rounds up";
    EXPECT_EQ(SvgBackgroundRasterSize({20.000002f, 20.0f}), (SvgRasterSize{20, 20})) << "float error is not a pixel";
    EXPECT_EQ(SvgBackgroundRasterSize({0.25f, 300.0f}), (SvgRasterSize{1, 300}));
    EXPECT_EQ(SvgBackgroundRasterSize({100000.0f, 100000.0f}),
              (SvgRasterSize{kMaxSvgBackgroundRasterEdge, kMaxSvgBackgroundRasterEdge}));
}

// Explicit sizes are CSS px, so the raster follows the content scale; a percent
// measures the positioning area; a missing axis keeps the image's aspect.
// Natural sizes are physical px: an SVG's intrinsic size x the content scale.
TEST(SvgBackgroundRaster, ExplicitSizeIsCssPixelsTimesContentScale)
{
    const BackgroundImageSize at1 = ComputeBackgroundImageSize(ExplicitSize(16, 16), 64, 64, 1.0f, 24, 24);
    EXPECT_FLOAT_EQ(at1.Width, 16.0f);
    EXPECT_FLOAT_EQ(at1.Height, 16.0f);

    const BackgroundImageSize at2 = ComputeBackgroundImageSize(ExplicitSize(16, 16), 128, 128, 2.0f, 48, 48);
    EXPECT_FLOAT_EQ(at2.Width, 32.0f);
    EXPECT_FLOAT_EQ(at2.Height, 32.0f);

    const BackgroundImageSize percent =
        ComputeBackgroundImageSize(ExplicitSize(50, 25, true, true), 200, 100, 2.0f, 48, 48);
    EXPECT_FLOAT_EQ(percent.Width, 100.0f);
    EXPECT_FLOAT_EQ(percent.Height, 25.0f);

    const BackgroundImageSize widthOnly = ComputeBackgroundImageSize(ExplicitSize(40, -1), 200, 200, 1.0f, 20, 10);
    EXPECT_FLOAT_EQ(widthOnly.Width, 40.0f);
    EXPECT_FLOAT_EQ(widthOnly.Height, 20.0f);
}

// Cover and Contain depend on the image's aspect alone; Auto is the natural
// size, shrunk only when the area is smaller.
TEST(SvgBackgroundRaster, FitModesFollowTheAspectAndAutoTheNaturalSize)
{
    const BackgroundImageSize contain = ComputeBackgroundImageSize(Mode(BackgroundSizeMode::Contain), 100, 100, 1.0f, 20, 10);
    EXPECT_FLOAT_EQ(contain.Width, 100.0f);
    EXPECT_FLOAT_EQ(contain.Height, 50.0f);

    const BackgroundImageSize cover = ComputeBackgroundImageSize(Mode(BackgroundSizeMode::Cover), 100, 100, 1.0f, 20, 10);
    EXPECT_FLOAT_EQ(cover.Width, 200.0f);
    EXPECT_FLOAT_EQ(cover.Height, 100.0f);

    const BackgroundImageSize autoFits = ComputeBackgroundImageSize(Mode(BackgroundSizeMode::Auto), 100, 100, 2.0f, 32, 32);
    EXPECT_FLOAT_EQ(autoFits.Width, 32.0f);
    EXPECT_FLOAT_EQ(autoFits.Height, 32.0f);

    const BackgroundImageSize autoShrinks = ComputeBackgroundImageSize(Mode(BackgroundSizeMode::Auto), 8, 100, 1.0f, 16, 16);
    EXPECT_FLOAT_EQ(autoShrinks.Width, 8.0f);
    EXPECT_FLOAT_EQ(autoShrinks.Height, 8.0f);
}

// A baked size stands in for a wanted one within a factor of two per axis.
TEST(SvgBackgroundRaster, CloseEnoughIsWithinTwiceOrHalfOnEachAxis)
{
    EXPECT_TRUE(IsSvgRasterSizeCloseEnough({20, 20}, {40, 40}));
    EXPECT_TRUE(IsSvgRasterSizeCloseEnough({40, 40}, {20, 20}));
    EXPECT_FALSE(IsSvgRasterSizeCloseEnough({20, 20}, {41, 41})) << "magnified past 2x";
    EXPECT_FALSE(IsSvgRasterSizeCloseEnough({41, 41}, {20, 20})) << "minified past 2x";
    EXPECT_FALSE(IsSvgRasterSizeCloseEnough({20, 20}, {20, 41})) << "either axis";
}

// While the drawn size bakes the draw binds the nearest baked size, measured
// in ratio on both axes so 16 is as near to 32 as 64 is.
TEST(SvgBackgroundRaster, NearestVariantIsByRatio)
{
    std::vector<SvgBackgroundVariant> variants(4);
    variants[0].Size = {16, 16};
    variants[1].Size = {64, 64};
    variants[2].Size = {256, 256};
    variants[3].Size = {64, 16};
    for (size_t i = 0; i < variants.size(); ++i)
        variants[i].Handle.id = static_cast<uint32_t>(i + 1);

    EXPECT_EQ(NearestSvgBackgroundVariant(variants, {20, 20}), 0u);
    EXPECT_EQ(NearestSvgBackgroundVariant(variants, {48, 48}), 1u);
    EXPECT_EQ(NearestSvgBackgroundVariant(variants, {192, 192}), 2u);
    EXPECT_EQ(NearestSvgBackgroundVariant(variants, {60, 15}), 3u) << "both axes count";

    variants[1].Handle = Rendering::INVALID_TEXTURE_HANDLE;
    EXPECT_EQ(NearestSvgBackgroundVariant(variants, {64, 64}), 3u) << "an unbaked slot is never bound";
    EXPECT_EQ(NearestSvgBackgroundVariant({}, {16, 16}), 0u);
}
