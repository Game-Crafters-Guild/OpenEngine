#include "TerrainGrass/GrassPlacementModel.h"

#include "Mathematics/Angle.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Terrain/TerrainTypes.h"

#include "GlslShim.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

// The classify kernel's per-cell box (grass_placement.glsl), compiled through GlslShim.h so the tests
// run the expressions the kernel runs.
namespace GrassCellBoxShader
{
using namespace GameEngine::GlslShim;
using GameEngine::GlslShim::abs;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::TerrainGrass::kGrassCellSize;
#include "GrassCellBoxExtracted.h"
} // namespace GrassCellBoxShader

// The vertex stage's blade (terrain_grass_vertex_modifier.glsl), to place a blade's vertices as it draws them.
namespace GrassCellBoxBladeShader
{
using namespace GameEngine::GlslShim;
using GameEngine::GlslShim::cos;
using GameEngine::GlslShim::cross;
using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::normalize;
using GameEngine::GlslShim::sin;
using GameEngine::GlslShim::smoothstep;
using GameEngine::GlslShim::sqrt;
#include "GrassBladeVertexExtracted.h"
} // namespace GrassCellBoxBladeShader

namespace GameEngine::TerrainGrass
{
namespace
{

using GroundFunction = std::function<float32(float32, float32)>;
using FrustumPlanes = std::array<Mathematics::Vector4, 6>;

constexpr float32 kBladeHeight = 0.6f;
const float32 kBladeReach = Terrain::TerrainGrassBladeReach(Terrain::TerrainGPUParams{});
constexpr float32 kDiscRadius = 160.0f;
constexpr int32 kDiscCells = static_cast<int32>(kDiscRadius / kGrassCellSize);

// The view's planes as ResolvePlaceParams hands them to the kernel: a reverse-Z perspective, 60 degrees
// vertical, at the Scene View's aspect.
FrustumPlanes PlanesForCamera(const Mathematics::Vector3& eye, float32 yawDegrees, float32 pitchDegrees)
{
    const float32 yaw = Mathematics::ToRadians(yawDegrees);
    const float32 pitch = Mathematics::ToRadians(pitchDegrees);
    const Mathematics::Vector3 forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                       std::cos(yaw) * std::cos(pitch));
    const Mathematics::Matrix4x4 view =
        Mathematics::MakeLookAtLH(eye, eye + forward, Mathematics::Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 proj =
        Mathematics::MakePerspectiveLH_ZO_ReverseZ(Mathematics::ToRadians(60.0f), 3067.0f / 893.0f, 0.1f, 5000.0f);
    FrustumPlanes planes{};
    Rendering::ExtractFrustumPlanes(proj * view, planes.data());
    return planes;
}

// terrain_grass_classify.comp's cellBoxOutsideFrustum. Rendering::TestAabbFrustum is the host twin of the
// kernel's AabbOutsideFrustum (frustum_tests.glsl).
bool CellBoxOutsideFrustum(const FrustumPlanes& planes, const GlslShim::vec2& cellMin, const GlslShim::vec2& groundRange,
                           float32 reach)
{
    const GlslShim::vec2 cellMax(cellMin.x + kGrassCellSize, cellMin.y + kGrassCellSize);
    const GrassCellBoxShader::GrassCellBox box = GrassCellBoxShader::GrassCellBladeBox(cellMin, cellMax, groundRange, reach);
    return !Rendering::TestAabbFrustum(Mathematics::Vector3(box.Min.x, box.Min.y, box.Min.z),
                                       Mathematics::Vector3(box.Max.x, box.Max.y, box.Max.z), planes.data());
}

// terrain_grass_classify.comp's cellOutsideFrustum for a cell whose ground is sampled.
bool CellOutsideFrustum(const FrustumPlanes& planes, float32 cellMinX, float32 cellMinZ, const GroundFunction& ground)
{
    const float32 cellMaxX = cellMinX + kGrassCellSize;
    const float32 cellMaxZ = cellMinZ + kGrassCellSize;
    const GlslShim::vec2 range = GrassCellBoxShader::GrassCellGroundRange(
        ground(cellMinX, cellMinZ), ground(cellMaxX, cellMinZ), ground(cellMinX, cellMaxZ), ground(cellMaxX, cellMaxZ),
        ground(cellMinX + kGrassCellSize * 0.5f, cellMinZ + kGrassCellSize * 0.5f));
    return CellBoxOutsideFrustum(planes, GlslShim::vec2(cellMinX, cellMinZ), range, kBladeReach);
}

bool PointInsideFrustum(const FrustumPlanes& planes, float32 x, float32 y, float32 z)
{
    for (const Mathematics::Vector4& plane : planes)
    {
        if (plane.x * x + plane.y * y + plane.z * z + plane.w < 0.0f)
            return false;
    }
    return true;
}

// True when a dense grid of the cell's ground and blade tips has a point inside the frustum.
bool CellGroundSeen(const FrustumPlanes& planes, float32 cellMinX, float32 cellMinZ, const GroundFunction& ground)
{
    constexpr int32 kSteps = 16;
    for (int32 stepX = 0; stepX <= kSteps; ++stepX)
    {
        for (int32 stepZ = 0; stepZ <= kSteps; ++stepZ)
        {
            const float32 x = cellMinX + kGrassCellSize * static_cast<float32>(stepX) / kSteps;
            const float32 z = cellMinZ + kGrassCellSize * static_cast<float32>(stepZ) / kSteps;
            const float32 y = ground(x, z);
            if (PointInsideFrustum(planes, x, y, z) || PointInsideFrustum(planes, x, y + kBladeHeight, z))
                return true;
        }
    }
    return false;
}

// Over the disc around the camera, counts the cells the box keeps and fails on any culled cell whose
// ground is in view.
int32 CountKeptCells(const FrustumPlanes& planes, const Mathematics::Vector3& eye, const GroundFunction& ground)
{
    const float32 baseX = std::floor(eye.x / kGrassCellSize) * kGrassCellSize;
    const float32 baseZ = std::floor(eye.z / kGrassCellSize) * kGrassCellSize;
    int32 kept = 0;
    for (int32 iz = -kDiscCells; iz <= kDiscCells; ++iz)
    {
        for (int32 ix = -kDiscCells; ix <= kDiscCells; ++ix)
        {
            const float32 cellMinX = baseX + static_cast<float32>(ix) * kGrassCellSize;
            const float32 cellMinZ = baseZ + static_cast<float32>(iz) * kGrassCellSize;
            if (!CellOutsideFrustum(planes, cellMinX, cellMinZ, ground))
                ++kept;
            else
                EXPECT_FALSE(CellGroundSeen(planes, cellMinX, cellMinZ, ground))
                    << "culled cell with ground in view at (" << cellMinX << ", " << cellMinZ << ")";
        }
    }
    return kept;
}

float32 FlatGround(float32, float32)
{
    return 0.0f;
}

// A 45 degree slope rising toward +X.
float32 SlopeGround(float32 x, float32)
{
    return x;
}

// A 40 m cliff along x = 60, one meter wide.
float32 CliffGround(float32 x, float32)
{
    const float32 t = std::clamp(x - 60.0f, 0.0f, 1.0f);
    return 40.0f * t * t * (3.0f - 2.0f * t);
}

float32 HillGround(float32 x, float32 z)
{
    return 18.0f * std::sin(x / 37.0f) * std::cos(z / 53.0f) + 6.0f * std::sin((x + z) / 11.0f);
}

TEST(GrassCellBox, ACellBehindThePitchedUpCameraIsCulled)
{
    const Mathematics::Vector3 eye(4.0f, 2.0f, 4.0f);
    const FrustumPlanes planes = PlanesForCamera(eye, 0.0f, 45.0f);
    EXPECT_TRUE(CellOutsideFrustum(planes, 0.0f, -48.0f, FlatGround));
}

TEST(GrassCellBox, ACellBesideTheFrustumIsCulled)
{
    const Mathematics::Vector3 eye(4.0f, 30.0f, 4.0f);
    const FrustumPlanes planes = PlanesForCamera(eye, 0.0f, -30.0f);
    EXPECT_TRUE(CellOutsideFrustum(planes, 136.0f, 16.0f, FlatGround));
    EXPECT_TRUE(CellOutsideFrustum(planes, -144.0f, 16.0f, FlatGround));
}

TEST(GrassCellBox, ACellOnAFortyFiveDegreeSlopeInViewIsKept)
{
    // Looking along the slope's contour: the cell ahead rises 8 m across its width.
    const Mathematics::Vector3 eye(4.0f, 6.0f, 4.0f);
    const FrustumPlanes planes = PlanesForCamera(eye, 0.0f, -10.0f);
    EXPECT_FALSE(CellOutsideFrustum(planes, 0.0f, 40.0f, SlopeGround));
    CountKeptCells(planes, eye, SlopeGround);
}

TEST(GrassCellBox, ACellUnderACliffIsKeptFromAboveAndBelow)
{
    // From the cliff top, looking down past its edge at the foot; and from the foot, looking up at it.
    const Mathematics::Vector3 top(76.0f, 42.0f, 4.0f);
    const FrustumPlanes fromTop = PlanesForCamera(top, -90.0f, -35.0f);
    EXPECT_FALSE(CellOutsideFrustum(fromTop, 48.0f, 0.0f, CliffGround));
    CountKeptCells(fromTop, top, CliffGround);

    const Mathematics::Vector3 foot(20.0f, 2.0f, 4.0f);
    const FrustumPlanes fromFoot = PlanesForCamera(foot, 90.0f, 30.0f);
    EXPECT_FALSE(CellOutsideFrustum(fromFoot, 56.0f, 0.0f, CliffGround));
    CountKeptCells(fromFoot, foot, CliffGround);
}

TEST(GrassCellBox, TheKeptCellsFollowTheYawAtAFixedPosition)
{
    const Mathematics::Vector3 eye(4.0f, 30.0f, 4.0f);
    const int32 discCells = (2 * kDiscCells + 1) * (2 * kDiscCells + 1);
    std::array<int32, 8> kept{};
    for (size_t step = 0; step < kept.size(); ++step)
    {
        const FrustumPlanes planes = PlanesForCamera(eye, 45.0f * static_cast<float32>(step), -20.0f);
        kept[step] = CountKeptCells(planes, eye, HillGround);
        EXPECT_GT(kept[step], 0);
        EXPECT_LT(kept[step], discCells / 2) << "yaw " << 45 * step;
    }
    EXPECT_NE(*std::min_element(kept.begin(), kept.end()), *std::max_element(kept.begin(), kept.end()));
}

TEST(GrassCellBox, TheGroundRangeHoldsEverySampleAndTheSlopePad)
{
    const GlslShim::vec2 flat = GrassCellBoxShader::GrassCellGroundRange(3.0f, 3.0f, 3.0f, 3.0f, 3.0f);
    EXPECT_FLOAT_EQ(flat.x, 3.0f - GrassCellBoxShader::kGrassCellSampleCover);
    EXPECT_FLOAT_EQ(flat.y, 3.0f + GrassCellBoxShader::kGrassCellSampleCover);

    // A 30 m step across one edge: the pad scales with the steepest sampled slope.
    const GlslShim::vec2 step = GrassCellBoxShader::GrassCellGroundRange(0.0f, 30.0f, 0.0f, 30.0f, 15.0f);
    EXPECT_LE(step.x, -15.0f);
    EXPECT_GE(step.y, 45.0f);
}

bool PlaneNormalXLess(const Mathematics::Vector4& a, const Mathematics::Vector4& b)
{
    return a.x < b.x;
}

// The X where the right side plane of a level camera looking along +Z crosses the line at depth z.
float32 RightPlaneEdgeX(const FrustumPlanes& planes, float32 z)
{
    const Mathematics::Vector4 right = *std::min_element(planes.begin(), planes.end(), PlaneNormalXLess);
    return -(right.z * z + right.w) / right.x;
}

// The vertex at height fraction t and edge `side` of a blade as the vertex stage builds it on flat ground.
GlslShim::vec3 BladeVertex(const GlslShim::vec3& root, const GlslShim::vec3& bendDir, const GlslShim::vec3& sideDir,
                           float32 tilt, float32 height, float32 t, float32 side, float32 halfWidth)
{
    const GlslShim::vec3 up(0.0f, 1.0f, 0.0f);
    GrassCellBoxBladeShader::GrassBladeFloor noFloor{};
    noFloor.MaxExpansion = 1.0f;
    const GlslShim::vec3 embedded(root.x, root.y + GrassCellBoxBladeShader::kGrassRootEmbed, root.z);
    return GrassCellBoxBladeShader::GrassBuildBladeVertex(embedded, up, bendDir, sideDir, tilt, 0.0f, 0.0f, t, height,
                                                          side, halfWidth, noFloor)
        .Position;
}

// A flat cell whose corner nearest the view is the root: the right plane's edge X grows with depth, so the
// whole cell is outside the right plane when the root is.
bool FlatCellAtRootOutside(const FrustumPlanes& planes, const GlslShim::vec3& root, float32 reach)
{
    const GlslShim::vec2 range = GrassCellBoxShader::GrassCellGroundRange(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    return CellBoxOutsideFrustum(planes, GlslShim::vec2(root.x, root.z - kGrassCellSize), range, reach);
}

TEST(GrassCellBox, TheReachReadsTheFactorsTheKernelsApply)
{
    EXPECT_EQ(Terrain::kTerrainGrassBladeHeightJitterMax, GrassCellBoxShader::kGrassBladeHeightJitterMax);
    EXPECT_EQ(Terrain::kTerrainGrassBladeWidthJitterMax, GrassCellBoxShader::kGrassBladeWidthJitterMax);
    EXPECT_EQ(Terrain::kTerrainGrassCardSizeJitterMax, GrassCellBoxShader::kGrassCardSizeJitterMax);
    EXPECT_EQ(Terrain::kTerrainGrassMaxBladeAspect, GrassCellBoxBladeShader::kGrassMaxBladeAspect);
    EXPECT_EQ(Terrain::kTerrainGrassMaxWidthExpansion, kGrassMaxWidthExpansion);
    EXPECT_EQ(Terrain::kTerrainGrassRootEmbedDepth, -GrassCellBoxBladeShader::kGrassRootEmbed);
}

TEST(GrassCellBox, ALeaningBladeRootedJustOutsideASidePlaneIsPlaced)
{
    const Mathematics::Vector3 eye(4.0f, 2.0f, 4.0f);
    const FrustumPlanes planes = PlanesForCamera(eye, 0.0f, 0.0f);
    const Terrain::TerrainGPUParams row{};
    // The tallest blade the place kernel makes for the row, at the largest chord angle, leaning into the view.
    const float32 height = row.GrassBladeHeight * (1.0f + row.GrassRandomScale) * Terrain::kTerrainGrassBladeHeightJitterMax
        * (1.0f + row.GrassClumpHeightVariance);
    const GlslShim::vec3 root(RightPlaneEdgeX(planes, 40.0f) + 0.3f, 0.0f, 40.0f);
    const GlslShim::vec3 tip = BladeVertex(root, GlslShim::vec3(-1.0f, 0.0f, 0.0f), GlslShim::vec3(0.0f, 0.0f, 1.0f),
                                           GrassCellBoxBladeShader::kGrassMaxTipChordRadians, height, 1.0f, 0.0f, 0.0f);
    ASSERT_FALSE(PointInsideFrustum(planes, root.x, root.y, root.z));
    ASSERT_TRUE(PointInsideFrustum(planes, tip.x, tip.y, tip.z));
    EXPECT_FALSE(FlatCellAtRootOutside(planes, root, Terrain::TerrainGrassBladeReach(row)));
}

TEST(GrassCellBox, AWideCardRootedOutsideASidePlaneIsPlaced)
{
    const Mathematics::Vector3 eye(4.0f, 2.0f, 4.0f);
    const FrustumPlanes planes = PlanesForCamera(eye, 0.0f, 0.0f);
    Terrain::TerrainGPUParams row{};
    row.GrassEnabled |= Terrain::kTerrainGrassBitTextureCard;
    row.GrassTextureSize = 3.0f;
    // The widest card the place kernel makes for the row, its edge turned toward the view.
    const float32 size = row.GrassTextureSize * (1.0f + row.GrassRandomScale) * Terrain::kTerrainGrassCardSizeJitterMax;
    const GlslShim::vec3 root(RightPlaneEdgeX(planes, 40.0f) + 1.6f, 0.0f, 40.0f);
    const GlslShim::vec3 edge = BladeVertex(root, GlslShim::vec3(0.0f, 0.0f, 1.0f), GlslShim::vec3(-1.0f, 0.0f, 0.0f), 0.0f,
                                            size, 0.0f, 1.0f, 0.5f * size);
    ASSERT_FALSE(PointInsideFrustum(planes, root.x, root.y, root.z));
    ASSERT_TRUE(PointInsideFrustum(planes, edge.x, edge.y, edge.z));
    EXPECT_FALSE(FlatCellAtRootOutside(planes, root, Terrain::TerrainGrassBladeReach(row)));
}

} // namespace
} // namespace GameEngine::TerrainGrass
