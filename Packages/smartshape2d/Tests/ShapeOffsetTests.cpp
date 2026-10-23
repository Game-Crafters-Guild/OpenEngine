#include "SmartShape2D/ShapeCollision.h"
#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/SmartShape.h"
#include "Mathematics/MatrixOps.h"
#include "Spline/SplineData.h"

#include <cmath>

#include <gtest/gtest.h>

namespace GameEngine::SmartShape2D
{
namespace
{
Spline::SplineData OffsetRectangle()
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

float FrontArea(const Physics::MeshShapeDef& mesh)
{
    float result = 0.0f;
    for (size_t index = 0; index < mesh.indices.size(); index += 3)
    {
        const auto& first = mesh.vertices[mesh.indices[index]];
        const auto& second = mesh.vertices[mesh.indices[index + 1]];
        const auto& third = mesh.vertices[mesh.indices[index + 2]];
        if (first.z < 0.0f && second.z < 0.0f && third.z < 0.0f)
            result += std::abs((second.x - first.x) * (third.y - first.y) -
                               (second.y - first.y) * (third.x - first.x)) * 0.5f;
    }
    return result;
}
}

TEST(SmartShapeOffsets, FillInflationChangesTheFillWithoutMovingTheEdges)
{
    Components::SmartShape settings;
    settings.FillMeshOffset = 0.5f;
    const auto shape = BuildShape(OffsetRectangle(), settings);
    ASSERT_TRUE(shape.Error.empty());
    EXPECT_FLOAT_EQ(shape.Parts[0].MinBounds[0], -2.5f);
    EXPECT_FLOAT_EQ(shape.Parts[0].MaxBounds[1], 1.5f);
    EXPECT_FLOAT_EQ(shape.Outline[0].x, -2.0f);
}

TEST(SmartShapeOffsets, FilledAndHollowCollisionUseTheirOwnDimensions)
{
    const auto shape = BuildShape(OffsetRectangle(), {});
    Components::SmartShape settings;
    settings.CollisionOffset = 0.5f;
    EXPECT_NEAR(FrontArea(BuildCollision(shape, true, settings)), 15.0f, 0.0001f);
    settings.CollisionOffset = 0.0f;
    settings.CollisionWidth = 0.5f;
    settings.CollisionMode = Components::ShapeCollisionMode::Hollow;
    EXPECT_NEAR(FrontArea(BuildCollision(shape, true, settings)), 6.0f, 0.0001f);
}

TEST(SmartShapeOffsets, AbsoluteFillPositionFollowsWorldTranslation)
{
    const auto world = Mathematics::MakeTranslation({5.0f, 7.0f, 0.0f});
    Components::SmartShape settings;
    auto shape = BuildShape(OffsetRectangle(), settings, {}, {}, world);
    EXPECT_FLOAT_EQ(shape.Parts[0].Vertices[0].TexCoords[0], -2.0f);
    settings.FillAbsolutePosition = true;
    shape = BuildShape(OffsetRectangle(), settings, {}, {}, world);
    EXPECT_FLOAT_EQ(shape.Parts[0].Vertices[0].TexCoords[0], 3.0f);
    EXPECT_FLOAT_EQ(shape.Parts[0].Vertices[0].TexCoords[1], -6.0f);
}
}
