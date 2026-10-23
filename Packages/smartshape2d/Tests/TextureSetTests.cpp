#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/SmartShape.h"
#include "Spline/SplineData.h"

#include <cmath>

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, RandomSelectionIsStableAndCanReachEveryEntry)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    for (uint32 point = 0; point <= 128; ++point)
        spline.AddPoint({static_cast<float>(point), 0.0f, 0.0f}, 1.0f);
    std::vector<Components::SmartShapeTexture> textures(12);
    for (uint32 index = 0; index < textures.size(); ++index)
    {
        textures[index].Index = index;
        textures[index].Material.Set(GUID::Derive(GUID::Null(), "variant/" + std::to_string(index)));
    }
    Components::SmartShape settings;
    settings.RandomizeTextures = true;
    settings.TextureSeed = 127;
    const auto first = BuildShape(spline, settings, {}, textures);
    const auto repeated = BuildShape(spline, settings, {}, textures);
    settings.TextureSeed = 128;
    const auto changed = BuildShape(spline, settings, {}, textures);
    ASSERT_TRUE(first.Error.empty()) << first.Error;
    constexpr size_t kParts = static_cast<size_t>(ShapePart::Count);
    bool different = false;
    for (size_t index = 0; index < textures.size(); ++index)
    {
        const size_t part = kParts + index * kParts + static_cast<size_t>(ShapePart::Top);
        EXPECT_FALSE(first.Parts[part].Indices.empty());
        EXPECT_EQ(first.Parts[part].Indices, repeated.Parts[part].Indices);
        ASSERT_EQ(first.Parts[part].Vertices.size(), repeated.Parts[part].Vertices.size());
        for (size_t vertex = 0; vertex < first.Parts[part].Vertices.size(); ++vertex)
            EXPECT_FLOAT_EQ(first.Parts[part].Vertices[vertex].Position[0], repeated.Parts[part].Vertices[vertex].Position[0]);
        different |= first.Parts[part].Vertices.size() != changed.Parts[part].Vertices.size();
    }
    EXPECT_TRUE(different);
}

TEST(SmartShapeTextureSet, PointSelectionOverridesTheDefaultSet)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0.0f, 0.0f, 0.0f}, 1.0f);
    spline.AddPoint({3.0f, 0.0f, 0.0f}, 1.0f);
    spline.AddPoint({6.0f, 0.0f, 0.0f}, 1.0f);
    std::vector<Components::SmartShapeTexture> textures(3);
    textures[1].PointIndex = 1;
    textures[2].PointIndex = 1;
    Components::SmartShapeEdge edge;
    edge.PointIndex = 1;
    edge.TextureIndex = 1;
    const auto result = BuildShape(spline, {}, std::span(&edge, 1), textures);
    ASSERT_TRUE(result.Error.empty());
    constexpr size_t kParts = static_cast<size_t>(ShapePart::Count);
    const size_t top = static_cast<size_t>(ShapePart::Top);
    EXPECT_EQ(result.Parts[kParts + 1 + top].Indices.size(), 6u);
    EXPECT_TRUE(result.Parts[kParts * 2 + 1 + top].Indices.empty());
    EXPECT_EQ(result.Parts[kParts * 3 + 1 + top].Indices.size(), 6u);
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, CornerSpritesOnlyCoverTheUpstreamRightAngleWindow)
{
    Components::SmartShape settings;
    settings.InnerCornerMaterial.Set(GUID::Derive(GUID::Null(), "inner"));
    settings.OuterCornerMaterial.Set(GUID::Derive(GUID::Null(), "outer"));
    for (const float degrees : {79.0f, 81.0f, 99.0f, 101.0f})
    {
        Spline::SplineData spline;
        spline.Type = Spline::SplineType::Linear;
        spline.AddPoint({-4.0f, 0.0f, 0.0f}, 1.0f);
        spline.AddPoint({0.0f, 0.0f, 0.0f}, 1.0f);
        const float radians = degrees * 3.14159265358979323846f / 180.0f;
        spline.AddPoint({4.0f * std::cos(radians), 4.0f * std::sin(radians), 0.0f}, 1.0f);
        const auto result = BuildShape(spline, settings);
        ASSERT_TRUE(result.Error.empty());
        const size_t corners = result.Parts[static_cast<size_t>(ShapePart::InnerCorner)].Indices.size() +
                               result.Parts[static_cast<size_t>(ShapePart::OuterCorner)].Indices.size();
        EXPECT_EQ(corners, degrees > 80.0f && degrees < 100.0f ? 6u : 0u) << degrees;
    }
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, EdgeRulesAndHiddenGapsStartIndependentTaperedRuns)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    for (uint32 point = 0; point < 4; ++point)
        spline.AddPoint({static_cast<float>(point) * 4, 0, 0}, 1);
    Components::SmartShape settings;
    settings.StartCapMaterial.Set(GUID::Derive(GUID::Null(), "start"));
    settings.EndCapMaterial.Set(GUID::Derive(GUID::Null(), "end"));
    settings.StartTaperSize = {0.25f, 0.25f};
    settings.EndTaperSize = {0.25f, 0.25f};
    Components::SmartShapeEdge rule;
    rule.PointIndex = 1;
    for (const bool visible : {true, false})
    {
        rule.Visible = visible;
        const auto result = BuildShape(spline, settings, std::span(&rule, 1));
        ASSERT_TRUE(result.Error.empty());
        const size_t runs = visible ? 3 : 2;
        EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::StartCap)].Indices.size(), runs * 6);
        EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::EndCap)].Indices.size(), runs * 6);
    }
}
}
