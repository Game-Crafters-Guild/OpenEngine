// The light-grid geometry of cluster_projection.glsl and the depth linearization of reverse_z.glsl,
// run on the host through GlslShim.h with the camera's own projection builders: the tile side
// planes of clustered_light_cull.comp and the view z that the cull reads.

#include <gtest/gtest.h>

#include "Engine/Rendering/CameraUtils.h"
#include "GlslShim.h"
#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::GlslShim::ClusterProjection
{
#include "Shaders/Includes/cluster_projection.glsl"
#include "Shaders/Includes/reverse_z.glsl"
} // namespace GameEngine::GlslShim::ClusterProjection

namespace
{
using GameEngine::GlslShim::vec3;
using GameEngine::GlslShim::vec4;
using GameEngine::GlslShim::ClusterProjection::GE_ClusterPlaneDistance;
using GameEngine::GlslShim::ClusterProjection::GE_ClusterTilePlanes;
using GameEngine::GlslShim::ClusterProjection::GE_ClusterTilePlanesFromCorners;
using GameEngine::GlslShim::ClusterProjection::LinearizeReverseZ;
using GameEngine::Mathematics::Matrix4x4;
using GameEngine::Mathematics::Vector4;

constexpr float kNearZ = 0.1f;
constexpr float kFarZ = 3000.0f;
constexpr float kAspect = 16.0f / 9.0f;
constexpr int kClustersX = 16;
constexpr int kClustersY = 9;
constexpr float kLightRadius = 0.25f;
constexpr float kViewZRelativeTolerance = 1e-5f;

// The cull's unprojection: NDC with reverse-Z depth (1 near, 0 far) to view space.
vec3 Unproject(const Matrix4x4& inverseProjection, float ndcX, float ndcY, float ndcZ)
{
    const Vector4 point = inverseProjection.Transform(Vector4(ndcX, ndcY, ndcZ, 1.0f));
    const float divisor = std::max(std::abs(point.w), 1e-6f);
    return {point.x / divisor, point.y / divisor, point.z / divisor};
}

// The tile's NDC rectangle and its four side planes, from the corners that clustered_light_cull.comp
// unprojects.
struct Tile
{
    float X0 = 0.0f;
    float X1 = 0.0f;
    float Y0 = 0.0f;
    float Y1 = 0.0f;
    GE_ClusterTilePlanes Planes;
};

Tile BuildTile(const Matrix4x4& inverseProjection, int tileX, int tileY)
{
    Tile tile;
    tile.X0 = static_cast<float>(tileX) / kClustersX * 2.0f - 1.0f;
    tile.X1 = static_cast<float>(tileX + 1) / kClustersX * 2.0f - 1.0f;
    tile.Y0 = 1.0f - static_cast<float>(tileY + 1) / kClustersY * 2.0f;
    tile.Y1 = 1.0f - static_cast<float>(tileY) / kClustersY * 2.0f;

    const vec3 farTL = Unproject(inverseProjection, tile.X0, tile.Y1, 0.0f);
    const vec3 farTR = Unproject(inverseProjection, tile.X1, tile.Y1, 0.0f);
    const vec3 farBL = Unproject(inverseProjection, tile.X0, tile.Y0, 0.0f);
    const vec3 farBR = Unproject(inverseProjection, tile.X1, tile.Y0, 0.0f);
    const vec3 nearTL = Unproject(inverseProjection, tile.X0, tile.Y1, 1.0f);
    const vec3 nearBR = Unproject(inverseProjection, tile.X1, tile.Y0, 1.0f);
    tile.Planes = GE_ClusterTilePlanesFromCorners(farTL, farTR, farBL, farBR, nearTL, nearBR);
    return tile;
}

bool SphereTouchesTile(const GE_ClusterTilePlanes& planes, const vec3& center, float radius)
{
    return GE_ClusterPlaneDistance(planes.Left, center) >= -radius
        && GE_ClusterPlaneDistance(planes.Right, center) >= -radius
        && GE_ClusterPlaneDistance(planes.Top, center) >= -radius
        && GE_ClusterPlaneDistance(planes.Bottom, center) >= -radius;
}

// Every tile keeps a light at its center and at each corner at near, middle and far view depths, and
// rejects a light one tile past each edge.
void ExpectTilesBoundTheirLights(const Matrix4x4& projection)
{
    const Matrix4x4 inverseProjection = GameEngine::Mathematics::Inverse(projection);
    for (int tileY = 0; tileY < kClustersY; ++tileY)
    {
        for (int tileX = 0; tileX < kClustersX; ++tileX)
        {
            SCOPED_TRACE(::testing::Message() << "tile " << tileX << ", " << tileY);
            const Tile tile = BuildTile(inverseProjection, tileX, tileY);
            const float centerX = (tile.X0 + tile.X1) * 0.5f;
            const float centerY = (tile.Y0 + tile.Y1) * 0.5f;
            const float width = tile.X1 - tile.X0;
            const float height = tile.Y1 - tile.Y0;
            for (const float viewZ : {1.0f, 50.0f, 1500.0f})
            {
                const Vector4 clip = projection.Transform(Vector4(0.0f, 0.0f, viewZ, 1.0f));
                const float ndcZ = clip.z / clip.w;
                const vec3 center = Unproject(inverseProjection, centerX, centerY, ndcZ);
                EXPECT_TRUE(SphereTouchesTile(tile.Planes, center, kLightRadius)) << "view z " << viewZ;
                for (const float cornerX : {tile.X0, tile.X1})
                {
                    for (const float cornerY : {tile.Y0, tile.Y1})
                    {
                        const vec3 corner = Unproject(inverseProjection, cornerX, cornerY, ndcZ);
                        EXPECT_TRUE(SphereTouchesTile(tile.Planes, corner, kLightRadius))
                            << "corner " << cornerX << ", " << cornerY << ", view z " << viewZ;
                    }
                }
                const vec3 pastLeft = Unproject(inverseProjection, tile.X0 - width, centerY, ndcZ);
                const vec3 pastRight = Unproject(inverseProjection, tile.X1 + width, centerY, ndcZ);
                const vec3 pastTop = Unproject(inverseProjection, centerX, tile.Y1 + height, ndcZ);
                const vec3 pastBottom = Unproject(inverseProjection, centerX, tile.Y0 - height, ndcZ);
                EXPECT_LT(GE_ClusterPlaneDistance(tile.Planes.Left, pastLeft), 0.0f) << "view z " << viewZ;
                EXPECT_LT(GE_ClusterPlaneDistance(tile.Planes.Right, pastRight), 0.0f) << "view z " << viewZ;
                EXPECT_LT(GE_ClusterPlaneDistance(tile.Planes.Top, pastTop), 0.0f) << "view z " << viewZ;
                EXPECT_LT(GE_ClusterPlaneDistance(tile.Planes.Bottom, pastBottom), 0.0f) << "view z " << viewZ;
            }
        }
    }
}

// The view z of a reverse-Z depth, read as the cull and the Forward+ lighting pass read it: through
// LinearizeReverseZ with the ge_depthTerms that the renderer uploads.
float ReadViewZ(const Matrix4x4& projection, float depth)
{
    const auto terms = GameEngine::Engine::Renderer::ProjectionDepthTerms::Of(projection);
    return LinearizeReverseZ(depth, vec4(terms.M22, terms.M32, terms.M23, terms.M33));
}

// A point at each view z, projected to depth and read back. The error may reach the larger of the
// relative tolerance and absoluteTolerance.
void ExpectViewZFromDepthRoundTrips(const Matrix4x4& projection, float absoluteTolerance)
{
    for (const float viewZ : {kNearZ, 1.0f, 50.0f, 1500.0f, kFarZ})
    {
        const Vector4 clip = projection.Transform(Vector4(0.0f, 0.0f, viewZ, 1.0f));
        const float readBack = ReadViewZ(projection, clip.z / clip.w);
        const float tolerance = std::max(viewZ * kViewZRelativeTolerance, absoluteTolerance);
        EXPECT_NEAR(readBack, viewZ, tolerance) << "view z " << viewZ;
    }
}

Matrix4x4 OrthographicProjection()
{
    constexpr float kOrthographicHeight = 40.0f;
    const float halfHeight = kOrthographicHeight * 0.5f;
    const float halfWidth = halfHeight * kAspect;
    return GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ(
        -halfWidth, halfWidth, -halfHeight, halfHeight, kNearZ, kFarZ);
}

Matrix4x4 PerspectiveProjection()
{
    return GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(
        GameEngine::Mathematics::ToRadians(60.0f), kAspect, kNearZ, kFarZ);
}

// The perspective projection with its far plane at infinity: depth = near / view z.
Matrix4x4 InfiniteFarPerspectiveProjection()
{
    Matrix4x4 projection = PerspectiveProjection();
    projection[2][2] = 0.0f;
    projection[3][2] = kNearZ;
    return projection;
}
} // namespace

TEST(ClusterProjection, OrthographicTilesBoundTheirLights)
{
    ExpectTilesBoundTheirLights(OrthographicProjection());
}

TEST(ClusterProjection, PerspectiveTilesBoundTheirLights)
{
    ExpectTilesBoundTheirLights(PerspectiveProjection());
}

TEST(ClusterProjection, PerspectiveTileSidesPassThroughTheEye)
{
    constexpr float kEyeOffsetTolerance = 1e-6f;
    const Matrix4x4 inverseProjection = GameEngine::Mathematics::Inverse(PerspectiveProjection());
    for (int tileY = 0; tileY < kClustersY; ++tileY)
    {
        for (int tileX = 0; tileX < kClustersX; ++tileX)
        {
            SCOPED_TRACE(::testing::Message() << "tile " << tileX << ", " << tileY);
            const GE_ClusterTilePlanes planes = BuildTile(inverseProjection, tileX, tileY).Planes;
            for (const vec4& plane : {planes.Left, planes.Right, planes.Top, planes.Bottom})
                EXPECT_NEAR(plane.w, 0.0f, kEyeOffsetTolerance);
        }
    }
}

TEST(ClusterProjection, OrthographicViewZFromDepthRoundTrips)
{
    // Orthographic depth is linear in view z: a float depth near 1 resolves view z only to steps of
    // (far - near) * 2^-24. The read-back holds to two steps.
    constexpr float kTwoDepthSteps = (kFarZ - kNearZ) * std::numeric_limits<float>::epsilon();
    ExpectViewZFromDepthRoundTrips(OrthographicProjection(), kTwoDepthSteps);
}

TEST(ClusterProjection, PerspectiveViewZFromDepthRoundTrips)
{
    ExpectViewZFromDepthRoundTrips(PerspectiveProjection(), 0.0f);
}

TEST(ClusterProjection, ViewZMatchesTheReverseZDepthDefinition)
{
    // Reverse-Z depth: orthographic d = (far - z) / (far - near), perspective d = near (far - z) / (z (far - near)).
    for (const float depth : {0.25f, 0.5f})
    {
        const float orthographicViewZ = kFarZ - depth * (kFarZ - kNearZ);
        EXPECT_NEAR(ReadViewZ(OrthographicProjection(), depth), orthographicViewZ,
            orthographicViewZ * kViewZRelativeTolerance) << "orthographic depth " << depth;
        const float perspectiveViewZ = kNearZ * kFarZ / (depth * (kFarZ - kNearZ) + kNearZ);
        EXPECT_NEAR(ReadViewZ(PerspectiveProjection(), depth), perspectiveViewZ,
            perspectiveViewZ * kViewZRelativeTolerance) << "perspective depth " << depth;
    }
}

TEST(ClusterProjection, ViewZAtDepthZeroIsTheFarPlane)
{
    // The cull's light-grid slice skip compares against this value: +infinity skips no slice.
    EXPECT_NEAR(ReadViewZ(PerspectiveProjection(), 0.0f), kFarZ, kFarZ * kViewZRelativeTolerance);
    EXPECT_EQ(ReadViewZ(InfiniteFarPerspectiveProjection(), 0.0f), std::numeric_limits<float>::infinity());
}
