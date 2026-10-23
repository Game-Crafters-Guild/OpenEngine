#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/SmartShape.h"
#include "Spline/SplineData.h"

#include <algorithm>
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
    textures[2].Index = 1;
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

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, ExplicitEdgeMaterialWinsOverTheDefaultTextureSet)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0, 0, 0}, 1);
    spline.AddPoint({4, 0, 0}, 1);
    Components::SmartShapeTexture texture;
    texture.Material.Set(GUID::Derive(GUID::Null(), "default texture"));
    Components::SmartShapeEdge edge;
    edge.Material.Set(GUID::Derive(GUID::Null(), "explicit edge"));
    const auto result = BuildShape(spline, {}, std::span(&edge, 1), std::span(&texture, 1));
    const size_t overridePart = static_cast<size_t>(ShapePart::Count);
    EXPECT_EQ(result.Parts[overridePart].Indices.size(), 6u);
    EXPECT_EQ(result.Materials[overridePart], edge.Material);
    EXPECT_TRUE(result.Parts[overridePart + 1 + static_cast<size_t>(ShapePart::Top)].Indices.empty());
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, OverlappingNormalRulesEachRenderAndPointOverridesReplaceAllLayers)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    for (uint32 point = 0; point < 4; ++point)
        spline.AddPoint({static_cast<float>(point) * 4, 0, 0}, 1);
    std::vector<Components::SmartShapeEdge> rules(3);
    for (size_t index = 0; index < 2; ++index)
    {
        rules[index].UseNormalRange = true;
        rules[index].NormalSweep = 360;
        rules[index].Material.Set(GUID::Derive(GUID::Null(), "layer/" + std::to_string(index)));
        rules[index].DepthOffset = -static_cast<float>(index);
    }
    rules[2].PointIndex = 1;
    rules[2].Material.Set(GUID::Derive(GUID::Null(), "point override"));
    const auto result = BuildShape(spline, {}, rules);
    ASSERT_TRUE(result.Error.empty());
    const size_t base = static_cast<size_t>(ShapePart::Count);
    EXPECT_EQ(result.Parts[base].Indices.size(), 12u);
    EXPECT_EQ(result.Parts[base + 1].Indices.size(), 12u);
    EXPECT_EQ(result.Parts[base + 2].Indices.size(), 6u);
    for (const auto& vertex : result.Parts[base + 1].Vertices)
        EXPECT_FLOAT_EQ(vertex.Position[2], -1.001f);
}

TEST(SmartShapeTextureSet, FlippedCornerWeldingPreservesEveryEdgeForBothWindings)
{
    for (const bool clockwise : {false, true})
    {
        for (const bool flip : {false, true})
        {
            Spline::SplineData spline;
            spline.Type = Spline::SplineType::Linear;
            spline.Closed = true;
            spline.AddPoint({-2, -2, 0}, 1);
            spline.AddPoint({2, -2, 0}, 1);
            spline.AddPoint({2, 2, 0}, 1);
            spline.AddPoint({-2, 2, 0}, 1);
            if (clockwise)
                std::reverse(spline.Points.begin(), spline.Points.end());
            Components::SmartShape settings;
            settings.FlipEdges = flip;
            settings.InnerCornerMaterial.Set(GUID::Derive(GUID::Null(), "inner"));
            settings.OuterCornerMaterial.Set(GUID::Derive(GUID::Null(), "outer"));
            const auto result = BuildShape(spline, settings);
            ASSERT_TRUE(result.Error.empty());
            for (size_t part = static_cast<size_t>(ShapePart::Top); part <= static_cast<size_t>(ShapePart::Left); ++part)
                EXPECT_EQ(result.Parts[part].Indices.size(), 6u) << clockwise << ' ' << flip;
        }
    }
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, LayersSelectIndependentSetsAndTextureOptions)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0, 0, 0}, 1);
    spline.AddPoint({3.3f, 0, 0}, 1);
    std::vector<Components::SmartShapeEdge> rules(2);
    std::vector<Components::SmartShapeTexture> textures(2);
    for (uint32 index = 0; index < 2; ++index)
    {
        rules[index].UseNormalRange = true;
        rules[index].NormalSweep = 360;
        rules[index].TextureSet = index;
        rules[index].OverrideTextureSettings = true;
        rules[index].StretchToFit = index == 0;
        rules[index].UseTaperTextures = index == 0;
        textures[index].TextureSet = index;
        textures[index].Material.Set(GUID::Derive(GUID::Null(), "texture/" + std::to_string(index)));
        textures[index].StartTaperMaterial.Set(GUID::Derive(GUID::Null(), "taper"));
        textures[index].StartTaperSize = {0.2f, 0.2f};
    }
    const auto result = BuildShape(spline, {}, rules, textures);
    ASSERT_TRUE(result.Error.empty());
    const size_t parts = static_cast<size_t>(ShapePart::Count);
    const size_t first = parts + rules.size();
    const size_t second = first + parts;
    ASSERT_EQ(result.Parts[first + static_cast<size_t>(ShapePart::Top)].Vertices.size(), 4u);
    EXPECT_FLOAT_EQ(result.Parts[first + static_cast<size_t>(ShapePart::Top)].Vertices[1].TexCoords[0], 3.0f);
    ASSERT_EQ(result.Parts[second + static_cast<size_t>(ShapePart::Top)].Vertices.size(), 4u);
    EXPECT_FLOAT_EQ(result.Parts[second + static_cast<size_t>(ShapePart::Top)].Vertices[1].TexCoords[0], 3.3f);
    EXPECT_EQ(result.Parts[first + static_cast<size_t>(ShapePart::StartCap)].Indices.size(), 6u);
    EXPECT_TRUE(result.Parts[second + static_cast<size_t>(ShapePart::StartCap)].Indices.empty());
}

TEST(SmartShapeTextureSet, NormalRangesUseTheAuthoredSegmentAcrossCurvedSamples)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::CubicBezier;
    spline.AddPoint({0, 0, 0}, 1);
    spline.AddPoint({4, 0, 0}, 1);
    spline.Points[0].TangentOut = {0, 4, 0};
    spline.Points[1].TangentIn = {0, 4, 0};
    Components::SmartShape settings;
    settings.WeldEdges = false;
    Components::SmartShapeEdge rule;
    rule.UseNormalRange = true;
    rule.NormalStart = 89;
    rule.NormalSweep = 2;
    rule.Material.Set(GUID::Derive(GUID::Null(), "curve material"));
    const auto result = BuildShape(spline, settings, std::span(&rule, 1));
    ASSERT_TRUE(result.Error.empty());
    EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::Count)].Indices.size(), settings.CurveSubdivisions * 6);
    for (size_t part = 1; part <= 4; ++part)
        EXPECT_TRUE(result.Parts[part].Indices.empty());
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, ClosedRuleRunFitsAcrossThePointIndexSeam)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.Closed = true;
    spline.AddPoint({-2, -1, 0}, 1);
    spline.AddPoint({2, -1, 0}, 1);
    spline.AddPoint({2, 1, 0}, 1);
    spline.AddPoint({-2, 1, 0}, 1);
    Components::SmartShape settings;
    settings.WeldEdges = false;
    settings.StretchToFit = false;
    settings.TopMaterial.Set(GUID::Derive(GUID::Null(), "edge"));
    settings.RightMaterial = settings.BottomMaterial = settings.LeftMaterial = settings.TopMaterial;
    Components::SmartShapeEdge rule;
    rule.PointIndex = 2;
    rule.Visible = false;
    const auto result = BuildShape(spline, settings, std::span(&rule, 1));
    ASSERT_TRUE(result.Error.empty());
    const auto& left = result.Parts[static_cast<size_t>(ShapePart::Left)];
    const auto& bottom = result.Parts[static_cast<size_t>(ShapePart::Bottom)];
    const auto& right = result.Parts[static_cast<size_t>(ShapePart::Right)];
    ASSERT_EQ(left.Vertices.size(), 4u);
    ASSERT_EQ(bottom.Vertices.size(), 4u);
    ASSERT_EQ(right.Vertices.size(), 4u);
    EXPECT_FLOAT_EQ(left.Vertices[0].TexCoords[0], 0);
    EXPECT_FLOAT_EQ(left.Vertices[1].TexCoords[0], bottom.Vertices[0].TexCoords[0]);
    EXPECT_FLOAT_EQ(bottom.Vertices[1].TexCoords[0], right.Vertices[0].TexCoords[0]);
    EXPECT_FLOAT_EQ(right.Vertices[1].TexCoords[0], 8);
}
}

namespace GameEngine::SmartShape2D
{
TEST(SmartShapeTextureSet, AuthoredIndicesDetermineSelectionAndDuplicatesAreRejected)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0, 0, 0}, 1);
    spline.AddPoint({4, 0, 0}, 1);
    std::vector<Components::SmartShapeTexture> textures(2);
    textures[0].Index = 5;
    textures[1].Index = 2;
    const auto result = BuildShape(spline, {}, {}, textures);
    ASSERT_TRUE(result.Error.empty());
    const size_t parts = static_cast<size_t>(ShapePart::Count);
    EXPECT_TRUE(result.Parts[parts + static_cast<size_t>(ShapePart::Top)].Indices.empty());
    EXPECT_EQ(result.Parts[parts * 2 + static_cast<size_t>(ShapePart::Top)].Indices.size(), 6u);
    textures[0].Index = 2;
    EXPECT_FALSE(BuildShape(spline, {}, {}, textures).Error.empty());
}
}
