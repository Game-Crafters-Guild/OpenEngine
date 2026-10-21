// RenderLayerMask: the one type every renderer, view and volume mask uses, and its scene text.

#include "Rendering/RenderLayerMask.h"
#include "Scene/RenderLayerMaskText.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::Rendering::RenderLayerMask;

TEST(RenderLayerMask, FromLayerSetsOneBit)
{
    EXPECT_EQ(RenderLayerMask::FromLayer(0), RenderLayerMask::Default());
    EXPECT_EQ(RenderLayerMask::FromLayer(5).Bits, 1u << 5);
    EXPECT_EQ(RenderLayerMask::FromLayer(31).Bits, 0x80000000u);
}

TEST(RenderLayerMask, ContainsReadsOneLayer)
{
    const RenderLayerMask mask = RenderLayerMask::FromLayer(1) | RenderLayerMask::FromLayer(2);
    EXPECT_FALSE(mask.Contains(0));
    EXPECT_TRUE(mask.Contains(1));
    EXPECT_TRUE(mask.Contains(2));
    EXPECT_FALSE(mask.Contains(3));
    EXPECT_FALSE(RenderLayerMask::All().Contains(32)) << "there is no layer 32";
}

TEST(RenderLayerMask, IntersectsNeedsASharedLayer)
{
    const RenderLayerMask a = RenderLayerMask::FromLayer(1) | RenderLayerMask::FromLayer(4);
    EXPECT_TRUE(a.Intersects(RenderLayerMask::FromLayer(4)));
    EXPECT_FALSE(a.Intersects(RenderLayerMask::FromLayer(0)));
    EXPECT_FALSE(a.Intersects(RenderLayerMask::None()));
    EXPECT_TRUE(a.Intersects(RenderLayerMask::All()));
}

TEST(RenderLayerMask, OrCombinesLayers)
{
    EXPECT_EQ((RenderLayerMask::FromLayer(0) | RenderLayerMask::FromLayer(3)).Bits, 0b1001u);
    EXPECT_EQ(RenderLayerMask::None() | RenderLayerMask::Default(), RenderLayerMask::Default());
}

TEST(RenderLayerMask, TextRoundTripsThroughTheSceneValueParser)
{
    const RenderLayerMask masks[] = {RenderLayerMask::Default(), RenderLayerMask::FromLayer(1) | RenderLayerMask::FromLayer(2),
                                     RenderLayerMask::FromLayer(31), RenderLayerMask::All(), RenderLayerMask::None()};
    for (const RenderLayerMask mask : masks)
    {
        const std::string text = GameEngine::Scene::FormatRenderLayerMask(mask);
        RenderLayerMask read = RenderLayerMask::FromLayer(7);
        std::string error;
        ASSERT_TRUE(GameEngine::Scene::ParseRenderLayerMask(text, "renderLayers", read, &error)) << text << ": " << error;
        EXPECT_EQ(read, mask) << text;
    }
    EXPECT_EQ(GameEngine::Scene::FormatRenderLayerMask(RenderLayerMask::Default()), "[0]");
    EXPECT_EQ(GameEngine::Scene::FormatRenderLayerMask(RenderLayerMask::FromLayer(2) | RenderLayerMask::FromLayer(1)), "[1, 2]");
}

TEST(RenderLayerMask, AllAndNoneAreWords)
{
    EXPECT_EQ(GameEngine::Scene::FormatRenderLayerMask(RenderLayerMask::All()), "all");
    EXPECT_EQ(GameEngine::Scene::FormatRenderLayerMask(RenderLayerMask::None()), "none");
    RenderLayerMask read;
    ASSERT_TRUE(GameEngine::Scene::ParseRenderLayerMask("all", "visibleLayers", read, nullptr));
    EXPECT_EQ(read, RenderLayerMask::All());
    ASSERT_TRUE(GameEngine::Scene::ParseRenderLayerMask("none", "visibleLayers", read, nullptr));
    EXPECT_EQ(read, RenderLayerMask::None());
}

TEST(RenderLayerMask, TextOutsideTheFormsIsRefusedWithTheFix)
{
    for (const char* text : {"[32]", "[-1]", "1", "[2, 1]", "[1, 1]", "[]", "everything", "[1.5]", "\"[0]\""})
    {
        RenderLayerMask read = RenderLayerMask::FromLayer(7);
        std::string error;
        EXPECT_FALSE(GameEngine::Scene::ParseRenderLayerMask(text, "renderLayers", read, &error)) << text;
        EXPECT_EQ(read, RenderLayerMask::FromLayer(7)) << text << ": a refused value leaves the field alone";
        EXPECT_NE(error.find("renderLayers takes layer numbers 0 to 31 in brackets, in ascending order and each once, or all, "
                             "or none"),
                  std::string::npos)
            << error;
    }
}

TEST(RenderLayerMask, ARefusedLongValueIsQuotedShort)
{
    std::string text = "[0";
    for (int i = 0; i < 1000; ++i)
        text += ", 1";
    text += "]";
    GameEngine::Rendering::RenderLayerMask read;
    std::string error;
    EXPECT_FALSE(GameEngine::Scene::ParseRenderLayerMask(text, "renderLayers", read, &error));
    EXPECT_LT(error.size(), 256u) << error;
    EXPECT_NE(error.find("...'"), std::string::npos) << error;
}
