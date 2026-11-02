#include "Paving/PavingMesh.h"

#include "SplineGeometry/AreaBoolean.h"
#include "SplineGeometry/AreaGround.h"

#include <gtest/gtest.h>

#include <cmath>

namespace GameEngine::SplineRecipes
{
namespace
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector2;
using Mathematics::Vector3;

// Ground that rises 1 m in 10 toward +X and 1 m in 20 toward +Z: a sloped square's rim.
constexpr float32 kSlopeX = 0.1f;
constexpr float32 kSlopeZ = 0.05f;

float32 SlopedGround(const Vector2& xz)
{
    return kSlopeX * xz.x + kSlopeZ * xz.y;
}

SG::AreaGround SlopedSquare()
{
    SG::AreaPiece square;
    square.Outer = {Vector2(-10.0f, -10.0f), Vector2(10.0f, -10.0f), Vector2(10.0f, 10.0f), Vector2(-10.0f, 10.0f)};
    const SG::AreaPiece pieces[] = {square};
    return SG::BuildAreaGround(pieces, SG::AreaGroundSettings{}, SlopedGround);
}

TEST(PavingMeshTests, OnASlopedRimThePavingFollowsTheGroundWithNoCrease)
{
    const PavingLidMesh lid =
        BuildPavingLidMesh(SlopedSquare(), {}, {}, Mathematics::Matrix4x4::Identity(), Vector2(0.0f, 0.0f));

    ASSERT_GT(lid.Indices.size(), 6u);
    // Every point, on the rim and inside, stands on the ground: the rim's height at the rim.
    for (const SG::SplineVertex& vertex : lid.Vertices)
        EXPECT_NEAR(vertex.Position.y, SlopedGround(Vector2(vertex.Position.x, vertex.Position.z)), 1.0e-4f);
    // One plane: every triangle faces up with the ground's normal, so no edge folds.
    const Vector3 groundNormal = Vector3(-kSlopeX, 1.0f, -kSlopeZ).Normalize();
    for (size_t t = 0; t + 2 < lid.Indices.size(); t += 3)
    {
        const Vector3& a = lid.Vertices[lid.Indices[t]].Position;
        const Vector3& b = lid.Vertices[lid.Indices[t + 1]].Position;
        const Vector3& c = lid.Vertices[lid.Indices[t + 2]].Position;
        const Vector3 face = Vector3::Cross(b - a, c - a).Normalize();
        EXPECT_GT(Vector3::Dot(face, groundNormal), 0.99999f);
    }
    for (const SG::SplineVertex& vertex : lid.Vertices)
        EXPECT_GT(Vector3::Dot(vertex.Normal, groundNormal), 0.99999f);
}

TEST(PavingMeshTests, TheSeedMovesThePatternAndTheOwnerTurnsIt)
{
    const SG::AreaGround ground = SlopedSquare();
    const PavingLidMesh plain =
        BuildPavingLidMesh(ground, {}, {}, Mathematics::Matrix4x4::Identity(), PavingPatternPhase(0));
    const PavingLidMesh seeded =
        BuildPavingLidMesh(ground, {}, {}, Mathematics::Matrix4x4::Identity(), PavingPatternPhase(3));
    // An owner turned a quarter turn about +Y: its x axis lies along world -Z.
    Mathematics::Matrix4x4 turned = Mathematics::Matrix4x4::Identity();
    turned.Data()[0] = 0.0f;
    turned.Data()[2] = -1.0f;
    turned.Data()[8] = 1.0f;
    turned.Data()[10] = 0.0f;
    const PavingLidMesh rotated = BuildPavingLidMesh(ground, {}, {}, turned, PavingPatternPhase(0));

    ASSERT_EQ(plain.Vertices.size(), seeded.Vertices.size());
    EXPECT_GT((seeded.Vertices[0].UV - plain.Vertices[0].UV).Length(), 0.01f);
    const Vector3 world = plain.Vertices[0].Position;
    EXPECT_NEAR(rotated.Vertices[0].UV.x, -world.z, 1.0e-4f);
    EXPECT_NEAR(rotated.Vertices[0].UV.y, world.x, 1.0e-4f);
}

} // namespace
TEST(PavingMeshTests, SeedsPastTheFloatsWholeNumbersKeepTheirOwnPhases)
{
    constexpr uint32 kLargeSeed = 1u << 30;
    const Mathematics::Vector2 first = PavingPatternPhase(kLargeSeed);
    const Mathematics::Vector2 next = PavingPatternPhase(kLargeSeed + 1u);
    EXPECT_GT((first - next).Length(), 0.01f);
    EXPECT_GE(first.x, 0.0f);
    EXPECT_LT(first.x, 64.0f);
}

} // namespace GameEngine::SplineRecipes
