#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/ShapeMesh.h"
#include "Mathematics/Geometry.h"
#include "SmartShape2D/SmartShape.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace GameEngine::SmartShape2D
{
namespace
{
Spline::SplineData Rectangle()
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.Closed = true;
    spline.AddPoint({-2.0f, -1.0f, 0.0f}, 1.0f);
    spline.AddPoint({2.0f, -1.0f, 0.0f}, 1.0f);
    spline.AddPoint({2.0f, 1.0f, 0.0f}, 1.0f);
    spline.AddPoint({-2.0f, 1.0f, 0.0f}, 1.0f);
    return spline;
}

float TriangleArea(const Mesh& mesh)
{
    float area = 0.0f;
    for (size_t i = 0; i < mesh.Indices.size(); i += 3)
    {
        const auto& a = mesh.Vertices[mesh.Indices[i]];
        const auto& b = mesh.Vertices[mesh.Indices[i + 1]];
        const auto& c = mesh.Vertices[mesh.Indices[i + 2]];
        area += std::abs((b.Position[0] - a.Position[0]) * (c.Position[1] - a.Position[1]) -
                         (b.Position[1] - a.Position[1]) * (c.Position[0] - a.Position[0])) * 0.5f;
    }
    return area;
}

void ExpectValidMesh(const Mesh& mesh)
{
    ASSERT_EQ(mesh.Indices.size() % 3, 0u);
    for (const auto index : mesh.Indices)
        ASSERT_LT(index, mesh.Vertices.size());
    for (const auto& vertex : mesh.Vertices)
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            EXPECT_TRUE(std::isfinite(vertex.Position[axis]));
            EXPECT_GE(vertex.Position[axis], mesh.MinBounds[axis]);
            EXPECT_LE(vertex.Position[axis], mesh.MaxBounds[axis]);
        }
        EXPECT_TRUE(std::isfinite(vertex.TexCoords[0]));
        EXPECT_TRUE(std::isfinite(vertex.TexCoords[1]));
        EXPECT_FLOAT_EQ(vertex.Normal[2], -1.0f);
    }
}

TEST(SmartShape2D, ClosedRectangleHasFillAndFourDirectionalEdges)
{
    const auto result = BuildShape(Rectangle(), {});
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    EXPECT_EQ(result.Parts[0].Indices.size(), 6u);
    EXPECT_FLOAT_EQ(TriangleArea(result.Parts[0]), 8.0f);
    for (size_t i = 1; i <= 4; ++i)
        EXPECT_EQ(result.Parts[i].Indices.size(), 6u);
    for (const auto& mesh : result.Parts)
        ExpectValidMesh(mesh);
}

TEST(SmartShape2D, ReversingWindingPreservesDirectionalMaterialAssignment)
{
    auto spline = Rectangle();
    std::reverse(spline.Points.begin(), spline.Points.end());
    const auto result = BuildShape(spline, {});
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    EXPECT_FLOAT_EQ(TriangleArea(result.Parts[0]), 8.0f);
    for (const auto& vertex : result.Parts[static_cast<size_t>(ShapePart::Top)].Vertices)
        EXPECT_GT(vertex.Position[1], 0.0f);
    for (const auto& vertex : result.Parts[static_cast<size_t>(ShapePart::Left)].Vertices)
        EXPECT_LT(vertex.Position[0], 0.0f);
}

TEST(SmartShape2D, ConcaveFillPreservesNotchArea)
{
    auto spline = Rectangle();
    spline.Points.clear();
    for (const Mathematics::Vector3 point : {Mathematics::Vector3{0, 0, 0}, {3, 0, 0}, {3, 1, 0}, {1, 1, 0}, {1, 3, 0}, {0, 3, 0}})
        spline.AddPoint(point, 1.0f);
    const auto result = BuildShape(spline, {});
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    EXPECT_EQ(result.Parts[0].Indices.size(), 12u);
    EXPECT_FLOAT_EQ(TriangleArea(result.Parts[0]), 5.0f);
}

TEST(SmartShape2D, OpenPathHasNoFillAndSupportsBothCaps)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::Linear;
    spline.AddPoint({0, 0, 0}, 1.0f);
    spline.AddPoint({3, 0, 0}, 2.0f);
    Components::SmartShape settings;
    settings.StartCapMaterial.Set(GUID::Derive(GUID::Null(), "test/start"));
    settings.EndCapMaterial.Set(GUID::Derive(GUID::Null(), "test/end"));
    const auto result = BuildShape(spline, settings);
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    EXPECT_TRUE(result.Parts[0].Indices.empty());
    EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::StartCap)].Indices.size(), 6u);
    EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::EndCap)].Indices.size(), 6u);
    const auto& edge = result.Parts[static_cast<size_t>(ShapePart::Top)];
    EXPECT_FLOAT_EQ(edge.Vertices[0].Position[1], 0.25f);
    EXPECT_FLOAT_EQ(edge.Vertices[1].Position[1], 0.25f);
}

TEST(SmartShape2D, TwoPointClosedCurveBehavesAsOpenPath)
{
    auto spline = Rectangle();
    spline.Points.resize(2);
    const auto result = BuildShape(spline, {});
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    EXPECT_TRUE(result.Parts[0].Indices.empty());
    EXPECT_EQ(result.Outline.size(), 2u);
}

TEST(SmartShape2D, BezierHandlesProduceSampledCurvedEdges)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::CubicBezier;
    spline.AddPoint({0, 0, 0}, 1.0f);
    spline.AddPoint({2, 0, 0}, 1.0f);
    spline.Points[0].TangentOut = {0, 2, 0};
    spline.Points[1].TangentIn = {0, 2, 0};
    const auto result = BuildShape(spline, {});
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    ASSERT_EQ(result.Outline.size(), 9u);
    EXPECT_NEAR(result.Outline[4].x, 1.0f, 0.0001f);
    EXPECT_NEAR(result.Outline[4].y, 1.5f, 0.0001f);
    for (const auto& mesh : result.Parts)
        ExpectValidMesh(mesh);
}

TEST(SmartShape2D, CrossingOutlineFailsWithoutPartialGeometry)
{
    auto spline = Rectangle();
    std::swap(spline.Points[1], spline.Points[2]);
    const auto result = BuildShape(spline, {});
    EXPECT_FALSE(result.Error.empty());
    for (const auto& mesh : result.Parts)
        EXPECT_TRUE(mesh.Indices.empty());
}

TEST(SmartShape2D, NonFiniteAndOutOfPlaneInputIsRejected)
{
    auto spline = Rectangle();
    spline.Points[1].Position.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(BuildShape(spline, {}).Error.empty());
    spline = Rectangle();
    spline.Points[1].Position.z = 1.0f;
    EXPECT_FALSE(BuildShape(spline, {}).Error.empty());
    spline = Rectangle();
    spline.Points[1].Radius = -1.0f;
    EXPECT_FALSE(BuildShape(spline, {}).Error.empty());
}

TEST(SmartShape2D, InvalidDimensionsAndSamplingAreRejected)
{
    Components::SmartShape settings;
    settings.TileLength = 0.0f;
    EXPECT_FALSE(BuildShape(Rectangle(), settings).Error.empty());
    settings = {};
    settings.FillScale.x = 0.0f;
    EXPECT_FALSE(BuildShape(Rectangle(), settings).Error.empty());
    settings = {};
    settings.CurveSubdivisions = 0;
    EXPECT_FALSE(BuildShape(Rectangle(), settings).Error.empty());
    settings.CurveSubdivisions = 65;
    EXPECT_FALSE(BuildShape(Rectangle(), settings).Error.empty());
}

TEST(SmartShape2D, FillUvScaleOffsetAndRotationAreApplied)
{
    Components::SmartShape settings;
    settings.FillScale = {2.0f, 4.0f};
    settings.FillOffset = {1.0f, 2.0f};
    settings.FillRotation = 90.0f;
    const auto result = BuildShape(Rectangle(), settings);
    ASSERT_TRUE(result.Error.empty());
    EXPECT_NEAR(result.Parts[0].Vertices[0].TexCoords[0], -1.0f, 0.0001f);
    EXPECT_NEAR(result.Parts[0].Vertices[0].TexCoords[1], 0.0f, 0.0001f);
}

TEST(SmartShape2D, StretchFitsWholeTilesAndCropKeepsAuthoredLength)
{
    auto spline = Rectangle();
    spline.Closed = false;
    spline.Points.resize(2);
    Components::SmartShape settings;
    settings.TileLength = 1.5f;
    auto result = BuildShape(spline, settings);
    ASSERT_TRUE(result.Error.empty());
    EXPECT_FLOAT_EQ(result.Parts[1].Vertices[1].TexCoords[0], 3.0f);
    settings.StretchToFit = false;
    result = BuildShape(spline, settings);
    EXPECT_NEAR(result.Parts[1].Vertices[1].TexCoords[0], 4.0f / 1.5f, 0.0001f);
}

TEST(SmartShape2D, DisableFillAndEdgesProducesNoRenderGeometry)
{
    Components::SmartShape settings;
    settings.FillEnabled = false;
    settings.EdgesEnabled = false;
    const auto result = BuildShape(Rectangle(), settings);
    ASSERT_TRUE(result.Error.empty());
    for (const auto& mesh : result.Parts)
        EXPECT_TRUE(mesh.Indices.empty());
}

TEST(SmartShape2D, CornerMaterialsAreOptionalAndFollowConvexity)
{
    Components::SmartShape settings;
    settings.OuterCornerMaterial.Set(GUID::Derive(GUID::Null(), "test/corner"));
    const auto result = BuildShape(Rectangle(), settings);
    ASSERT_TRUE(result.Error.empty());
    EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::OuterCorner)].Indices.size(), 24u);
    EXPECT_TRUE(result.Parts[static_cast<size_t>(ShapePart::InnerCorner)].Indices.empty());
}
} // namespace
} // namespace GameEngine::SmartShape2D

namespace GameEngine::SmartShape2D
{
TEST(SmartShape2D, PointOverridesReplaceMaterialHideEdgesAndScaleWidths)
{
    Components::SmartShapeEdge edge;
    edge.PointIndex = 2;
    edge.Material.Set(GUID::Derive(GUID::Null(), "test/override"));
    edge.WidthScale = 2.0f;
    Components::SmartShape settings;
    settings.WeldEdges = false;
    auto result = BuildShape(Rectangle(), settings, std::span(&edge, 1));
    ASSERT_TRUE(result.Error.empty()) << result.Error;
    EXPECT_TRUE(result.Parts[static_cast<size_t>(ShapePart::Top)].Indices.empty());
    ASSERT_EQ(result.Parts[static_cast<size_t>(ShapePart::Count)].Indices.size(), 6u);
    EXPECT_EQ(result.Materials[static_cast<size_t>(ShapePart::Count)], edge.Material);
    EXPECT_FLOAT_EQ(result.Parts[static_cast<size_t>(ShapePart::Count)].Vertices[0].Position[1], 1.5f);
    edge.Visible = false;
    result = BuildShape(Rectangle(), {}, std::span(&edge, 1));
    EXPECT_TRUE(result.Parts[static_cast<size_t>(ShapePart::Count)].Indices.empty());
}

TEST(SmartShape2D, NormalRangesWrapAndPointOverridesTakePrecedence)
{
    Components::SmartShapeEdge range;
    range.UseNormalRange = true;
    range.NormalStart = 315;
    range.NormalSweep = 90;
    range.Material.Set(GUID::Derive(GUID::Null(), "test/right"));
    const auto result = BuildShape(Rectangle(), {}, std::span(&range, 1));
    ASSERT_TRUE(result.Error.empty());
    EXPECT_TRUE(result.Parts[static_cast<size_t>(ShapePart::Right)].Indices.empty());
    EXPECT_EQ(result.Parts[static_cast<size_t>(ShapePart::Count)].Indices.size(), 6u);
    Components::SmartShapeEdge point;
    point.PointIndex = 1;
    point.Visible = false;
    const std::array overrides{range, point};
    const auto hidden = BuildShape(Rectangle(), {}, overrides);
    EXPECT_TRUE(hidden.Parts[static_cast<size_t>(ShapePart::Count)].Indices.empty());
    EXPECT_TRUE(hidden.Parts[2].Indices.empty());
}
TEST(SmartShape2D, TightWeldedFlowerTrailFacesTheCameraAtEveryTriangle)
{
    Spline::SplineData spline;
    spline.Type = Spline::SplineType::CatmullRom;
    spline.Closed = false;
    for (const Mathematics::Vector3 point : {Mathematics::Vector3{-12, -1.4f, 0}, {-9, -0.2f, 0},
        {-6, -0.6f, 0}, {-4.8788586f, 1.8793976f, 0}, {0, 1.2f, 0}, {3, 3.1f, 0},
        {6, 2.7f, 0}, {9, 3.7f, 0}, {12, 3.2f, 0}})
        spline.AddPoint(point, 1.0f);
    Components::SmartShape settings;
    settings.FillEnabled = false;
    settings.EdgeWidth = 1.5f;
    settings.TileLength = 4.5f;
    settings.CurveSubdivisions = 16;
    settings.WeldEdges = true;
    const auto geometry = BuildShape(spline, settings);
    ASSERT_TRUE(geometry.Error.empty());
    size_t triangles = 0;
    for (const auto& mesh : geometry.Parts)
    {
        for (size_t index = 0; index < mesh.Indices.size(); index += 3)
        {
            const auto& a = mesh.Vertices[mesh.Indices[index]];
            const auto& b = mesh.Vertices[mesh.Indices[index + 1]];
            const auto& c = mesh.Vertices[mesh.Indices[index + 2]];
            const float area = (b.Position[0] - a.Position[0]) * (c.Position[1] - a.Position[1]) -
                (b.Position[1] - a.Position[1]) * (c.Position[0] - a.Position[0]);
            EXPECT_LE(area, 0.0f) << "Triangle " << triangles << " faces away from its -Z normal";
            ++triangles;
        }
    }
    EXPECT_GT(triangles, 0u);
}

TEST(SmartShape2D, ConcaveBorderQuadsCoverTheirInteriorWithoutOverlappingTriangles)
{
    std::array<Mathematics::Vector2, 4> points{{{0, 0}, {0.5f, 0.5f}, {2, 0}, {0, 2}}};
    for (int winding = 0; winding < 2; ++winding)
    {
        for (size_t corner = 0; corner < points.size(); ++corner)
        {
            Mesh mesh;
            AppendShapeQuad(mesh, points[0], points[1], points[2], points[3], 0, 1, 0, false);
            EXPECT_NEAR(TriangleArea(mesh), std::abs(Mathematics::PolygonDoubledSignedArea(points)) * 0.5f, 0.00001f);
            EXPECT_EQ(mesh.Indices.size(), 6u);
            std::rotate(points.begin(), points.begin() + 1, points.end());
        }
        std::reverse(points.begin(), points.end());
    }
}

}
