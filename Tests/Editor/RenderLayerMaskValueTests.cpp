// set_component's render layer values: a JSON list of layer numbers or the scene text, all and none; a
// raw number is refused with a message that names the forms the field takes.

#include "DebugServer/RenderLayerMaskValue.h"

#include <gtest/gtest.h>

#include <string>

using GameEngine::Editor::ReadRenderLayerMaskValue;
using GameEngine::Rendering::RenderLayerMask;
using nlohmann::json;

TEST(RenderLayerMaskValueTest, TakesTheListTheTextAllAndNone)
{
    RenderLayerMask layers;
    std::string error;
    ASSERT_TRUE(ReadRenderLayerMaskValue(json::array({1, 2}), "VisibleLayers", layers, error)) << error;
    EXPECT_EQ(layers.Bits, 0b110u);
    ASSERT_TRUE(ReadRenderLayerMaskValue(json("[0, 4]"), "VisibleLayers", layers, error)) << error;
    EXPECT_EQ(layers.Bits, 0b10001u);
    ASSERT_TRUE(ReadRenderLayerMaskValue(json("all"), "VisibleLayers", layers, error));
    EXPECT_EQ(layers, RenderLayerMask::All());
    ASSERT_TRUE(ReadRenderLayerMaskValue(json("none"), "VisibleLayers", layers, error));
    EXPECT_EQ(layers, RenderLayerMask::None());
}

TEST(RenderLayerMaskValueTest, ARawNumberIsRefusedWithTheAcceptedForms)
{
    RenderLayerMask layers = RenderLayerMask::FromLayer(3);
    std::string error;
    EXPECT_FALSE(ReadRenderLayerMaskValue(json(4294967295u), "VisibleLayers", layers, error));
    EXPECT_NE(error.find("VisibleLayers takes a list of layer numbers 0 to 31"), std::string::npos) << error;
    EXPECT_NE(error.find("\"all\""), std::string::npos) << error;
    EXPECT_EQ(layers, RenderLayerMask::FromLayer(3)) << "a refused value leaves the field alone";

    EXPECT_FALSE(ReadRenderLayerMaskValue(json::array({2, 1}), "VisibleLayers", layers, error))
        << "the list is in ascending order";
    EXPECT_FALSE(ReadRenderLayerMaskValue(json::array({32}), "VisibleLayers", layers, error));
}
