// The face file UIManager loads for a Roboto family when no font resolver is installed (the
// colour picker window, the UI examples and tests). It follows the resolver's CSS weight
// matching, so a weight draws with the same face on both paths.

#include "ShippedFontFace.h"

#include <gtest/gtest.h>
#include <string>

using GameEngine::UI::PickShippedFontFile;

namespace
{

std::string Upright(const char* family, int weight)
{
    const char* file = PickShippedFontFile(family, weight, false);
    return file ? file : "<none>";
}

} // namespace

TEST(ShippedFontFaceTests, RobotoWeightsPickRegularMediumAndBold)
{
    EXPECT_EQ(Upright("roboto", 400), "Roboto-Regular.ttf");
    EXPECT_EQ(Upright("roboto", 450), "Roboto-Medium.ttf");
    EXPECT_EQ(Upright("roboto", 500), "Roboto-Medium.ttf");
    EXPECT_EQ(Upright("roboto", 600), "Roboto-Bold.ttf");
    EXPECT_EQ(Upright("roboto", 700), "Roboto-Bold.ttf");
}

TEST(ShippedFontFaceTests, RobotoMonoHasNoMediumAndTakesBoldAbove500)
{
    EXPECT_EQ(Upright("roboto mono", 400), "RobotoMono-Regular.ttf");
    EXPECT_EQ(Upright("roboto mono", 450), "RobotoMono-Regular.ttf");
    EXPECT_EQ(Upright("roboto mono", 500), "RobotoMono-Regular.ttf");
    EXPECT_EQ(Upright("roboto mono", 600), "RobotoMono-Bold.ttf");
    EXPECT_EQ(Upright("roboto mono", 700), "RobotoMono-Bold.ttf");
}

TEST(ShippedFontFaceTests, ItalicPicksTheItalicFileOfTheSameWeight)
{
    EXPECT_STREQ(PickShippedFontFile("roboto", 600, true), "Roboto-BoldItalic.ttf");
    EXPECT_STREQ(PickShippedFontFile("roboto", 500, true), "Roboto-MediumItalic.ttf");
    EXPECT_STREQ(PickShippedFontFile("robotomono", 400, true), "RobotoMono-Italic.ttf");
}

TEST(ShippedFontFaceTests, AFamilyTheEditorDoesNotShipHasNoFile)
{
    EXPECT_EQ(PickShippedFontFile("inter", 500, false), nullptr);
}
