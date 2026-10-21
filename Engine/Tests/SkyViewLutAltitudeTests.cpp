// The sky-view LUT's altitude bands (Rendering/Sky/SkyViewLutAltitude.h): the LUT rebakes only when
// the camera crosses from one band into another, and band 0 bakes at the ground.

#include <gtest/gtest.h>

#include "Rendering/Sky/SkyViewLutAltitude.h"

using GameEngine::Rendering::SkyViewLutAltitudeBand;
using GameEngine::Rendering::SkyViewLutBandAltitude;

TEST(SkyViewLutAltitude, ACameraMovingInsideABandKeepsItsBand)
{
    EXPECT_EQ(SkyViewLutAltitudeBand(-50.0f), 0u);
    EXPECT_EQ(SkyViewLutAltitudeBand(0.0f), 0u);
    EXPECT_EQ(SkyViewLutAltitudeBand(249.0f), 0u);
    EXPECT_EQ(SkyViewLutAltitudeBand(250.0f), 1u);
    EXPECT_EQ(SkyViewLutAltitudeBand(999.0f), 3u);
    EXPECT_EQ(SkyViewLutAltitudeBand(1000.0f), 4u);
    EXPECT_EQ(SkyViewLutAltitudeBand(1180.0f), 4u);
    EXPECT_EQ(SkyViewLutAltitudeBand(1190.0f), 5u);
    EXPECT_FLOAT_EQ(SkyViewLutBandAltitude(0u), 0.0f);
}

TEST(SkyViewLutAltitude, AClimbToTheTopOfTheAtmosphereCrossesThirtyBands)
{
    EXPECT_EQ(SkyViewLutAltitudeBand(100000.0f), 30u);
    for (uint32_t band = 0; band <= 30u; ++band)
    {
        EXPECT_EQ(SkyViewLutAltitudeBand(SkyViewLutBandAltitude(band)), band) << "lower edge of band " << band;
        EXPECT_LT(SkyViewLutBandAltitude(band), SkyViewLutBandAltitude(band + 1u)) << "band " << band;
    }
}
