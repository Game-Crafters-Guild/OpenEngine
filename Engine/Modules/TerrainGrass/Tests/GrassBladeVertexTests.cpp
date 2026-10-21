// Behavioural tests for one grass blade vertex, executed on the SHIPPED shader code.
//
// The block of terrain_grass_vertex_modifier.glsl between the GE_SHARED_GRASS_BLADE_VERTEX markers
// is extracted verbatim at build time (ExtractShaderBlock.cmake) and compiled here through
// GlslShim.h, so an edit to the shader is an edit to what these tests measure.
//
// Two properties are under test. #4206: the ground colour a blade blends from is resolved ONCE per
// blade, at its root; a sample point that follows the vertex up the blade draws the ground's pattern
// up the blade as streaks, because a leaning blade's vertices stand over different ground. And the
// leaf's shape: the spine's angle law, the twist about the spine, the width profile, the rounded
// shading normal at the two edges and the rows the strip is built on.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include "TerrainGrass/GrassBladeRows.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace Shader
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
} // namespace Shader

using Shader::vec3;

constexpr float kBladeHeight = 0.72f;
constexpr float kHalfWidth = 0.043f;
// A strong gust on a tilted blade: the vertex modifier caps the bend at 1.15.
constexpr float kTilt = 0.25f;
constexpr float kBend = 1.0f;

const vec3 kUp(0.0f, 1.0f, 0.0f);
const vec3 kBendDir(0.8f, 0.0f, 0.6f);
const vec3 kSideDir(-0.6f, 0.0f, 0.8f);

// No pixel floor and the fold at full strength: the blade's own shape.
Shader::GrassBladeFloor NoFloor()
{
    Shader::GrassBladeFloor f;
    f.MinHalfWidth = 0.0f;
    f.MaxExpansion = 6.0f;
    f.RootPixels = 0.0f;
    f.CameraPos = vec3(0.0f, 1.5f, -20.0f);
    return f;
}

Shader::GrassBladeVertex BuildVertex(const vec3& root, float t, float side, float tilt = kTilt,
                                     float bend = kBend, float twistRadians = 0.0f)
{
    return Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, tilt, bend, twistRadians, t,
                                         kBladeHeight, side, kHalfWidth, NoFloor());
}

float AngleBetween(const vec3& a, const vec3& b)
{
    // atan2 of the cross and dot products, which keeps its precision where acos loses it near 0.
    return std::atan2(Shader::length(Shader::cross(a, b)), Shader::dot(a, b));
}

void ExpectSamePoint(const vec3& a, const vec3& b, const char* what, float t, float side)
{
    EXPECT_EQ(a.x, b.x) << what << " at t=" << t << " side=" << side;
    EXPECT_EQ(a.y, b.y) << what << " at t=" << t << " side=" << side;
    EXPECT_EQ(a.z, b.z) << what << " at t=" << t << " side=" << side;
}
} // namespace

TEST(GrassBladeVertex, EveryVertexResolvesItsGroundColourAtTheRoot)
{
    const vec3 root(12.25f, 30.5f, -7.75f);
    for (int step = 0; step <= 10; ++step)
    {
        const float t = static_cast<float>(step) / 10.0f;
        for (float side : {-1.0f, 0.0f, 1.0f})
        {
            const Shader::GrassBladeVertex v = BuildVertex(root, t, side, kTilt, kBend, 1.5f);
            ExpectSamePoint(v.GroundSamplePoint, root, "ground colour sample point", t, side);
        }
    }
}

// The control for the test above: the blade under test really does lean, so its vertices stand
// over different ground, and a sample point that followed them would move.
TEST(GrassBladeVertex, ALeaningBladeTipStandsOverDifferentGroundThanItsRoot)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    const Shader::GrassBladeVertex base = BuildVertex(root, 0.0f, 0.0f);
    const Shader::GrassBladeVertex tip = BuildVertex(root, 1.0f, 0.0f);
    ExpectSamePoint(base.Position, root, "the spine's base", 0.0f, 0.0f);
    const float driftXZ = std::sqrt((tip.Position.x - root.x) * (tip.Position.x - root.x) +
                                    (tip.Position.z - root.z) * (tip.Position.z - root.z));
    EXPECT_GT(driftXZ, 0.5f * kBladeHeight) << "a tilted, bent blade's tip stands well off its root";
}

// The spine leans from the terrain normal by tilt + bend t^2 + 0.6 bend t^4: a tilted base, a
// cantilever bend, and a tip that droops faster than a uniform curve.
TEST(GrassBladeVertex, TheSpineFollowsTheTiltBendAndDroopLaw)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    for (int step = 0; step <= 10; ++step)
    {
        const float t = static_cast<float>(step) / 10.0f;
        const float expected = kTilt + kBend * t * t + 0.6f * kBend * t * t * t * t;
        const Shader::GrassBladeVertex v = BuildVertex(root, t, 0.0f);
        EXPECT_NEAR(AngleBetween(v.Tangent, kUp), expected, 1e-4f) << "tangent angle at t=" << t;
        // The tangent leans in the bend plane, toward the bend direction.
        EXPECT_NEAR(Shader::dot(v.Tangent, kSideDir), 0.0f, 1e-5f) << "t=" << t;
        EXPECT_GE(Shader::dot(v.Tangent, kBendDir), 0.0f) << "t=" << t;
    }
    // The vertex itself lies on the arc: its chord from the root runs at the angle law's mean.
    const Shader::GrassBladeVertex tip = BuildVertex(root, 1.0f, 0.0f);
    const float chordMean = kTilt + kBend / 3.0f + 0.6f * kBend / 5.0f;
    EXPECT_NEAR(AngleBetween(tip.Position, kUp), chordMean, 1e-4f);
    EXPECT_NEAR(Shader::length(tip.Position), kBladeHeight, 1e-5f);
}

// The leaf's side turns about the spine by twist * t, and stays perpendicular to the spine however
// the blade leans relative to its face.
TEST(GrassBladeVertex, TheSideTwistsAboutTheSpineInProportionToHeight)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    const float twist = 0.5f * 3.14159265f; // a quarter turn
    for (int step = 0; step <= 10; ++step)
    {
        const float t = static_cast<float>(step) / 10.0f;
        const Shader::GrassBladeVertex left = BuildVertex(root, t, -1.0f, kTilt, kBend, twist);
        const Shader::GrassBladeVertex right = BuildVertex(root, t, 1.0f, kTilt, kBend, twist);
        const Shader::GrassBladeVertex flatLeft = BuildVertex(root, t, -1.0f, kTilt, kBend, 0.0f);
        const Shader::GrassBladeVertex flatRight = BuildVertex(root, t, 1.0f, kTilt, kBend, 0.0f);
        const vec3 across = right.Position - left.Position;
        const vec3 flatAcross = flatRight.Position - flatLeft.Position;
        EXPECT_NEAR(Shader::length(across), 2.0f * kHalfWidth, 1e-5f) << "t=" << t;
        EXPECT_NEAR(Shader::dot(Shader::normalize(across), left.Tangent), 0.0f, 1e-5f) << "t=" << t;
        EXPECT_NEAR(AngleBetween(across, flatAcross), twist * t, 2e-4f) << "t=" << t;
    }
}

// The lean plane points anywhere relative to the leaf's face (fan + wind + jitter), so the side
// must bend WITH the spine: whatever the angle between the side and the bend direction, the strip
// keeps its full width and stays perpendicular to the spine at every height.
TEST(GrassBladeVertex, TheSideBendsWithTheSpineWhateverTheLeanPlane)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    for (float sideAngle : {0.3f, 0.9f, 1.4f})
    {
        // A unit side in the ground plane, `sideAngle` from the bend direction (0.8, 0, 0.6).
        const vec3 side(0.8f * std::cos(sideAngle) - 0.6f * std::sin(sideAngle), 0.0f,
                        0.6f * std::cos(sideAngle) + 0.8f * std::sin(sideAngle));
        for (int step = 0; step <= 10; ++step)
        {
            const float t = static_cast<float>(step) / 10.0f;
            const auto left = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, side, kTilt, kBend, 0.0f, t,
                                                            kBladeHeight, -1.0f, kHalfWidth, NoFloor());
            const auto right = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, side, kTilt, kBend, 0.0f, t,
                                                             kBladeHeight, 1.0f, kHalfWidth, NoFloor());
            const vec3 across = right.Position - left.Position;
            EXPECT_NEAR(Shader::length(across), 2.0f * kHalfWidth, 1e-5f) << "angle " << sideAngle << " t=" << t;
            EXPECT_NEAR(Shader::dot(Shader::normalize(across), left.Tangent), 0.0f, 1e-5f)
                << "angle " << sideAngle << " t=" << t;
        }
    }
}

// The tilt is capped against the bend so the chord to the tip stays short of horizontal: no spine
// vertex goes below the root's ground plane, at the dial ceilings with the tuft fan at its edge and
// the bend at its gust cap.
TEST(GrassBladeVertex, TheCappedTiltKeepsEveryVertexAboveTheGround)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    for (float bend : {0.0f, 0.35f, 0.8f, 1.15f})
    {
        for (float tilt : {0.25f, 0.6f, 1.2f, 1.2f + 2.0f * 1.5f})
        {
            const float capped = Shader::GrassBladeCappedTilt(tilt, bend);
            EXPECT_LE(capped, tilt);
            for (int step = 0; step <= 10; ++step)
            {
                const float t = static_cast<float>(step) / 10.0f;
                const auto v = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, capped, bend, 0.0f,
                                                             t, kBladeHeight, 0.0f, 0.0f, NoFloor());
                EXPECT_GE(v.Position.y, 0.0f) << "tilt " << tilt << " bend " << bend << " t=" << t;
            }
        }
    }
    // The cap binds only where it must: a default blade in calm air keeps its tilt.
    EXPECT_EQ(Shader::GrassBladeCappedTilt(0.25f, 0.35f), 0.25f);
}

// The pixel floor holds the blade's PROJECTED width, vertex by vertex. A blade nearly edge-on at its
// root is widened there to stay a pixel wide; where the twist turns it face-on, the floor shrinks
// with the facing, so the strip never projects much wider than the floor asked for at the root.
TEST(GrassBladeVertex, ThePixelFloorFollowsTheTwistedFacing)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    // The camera 10 m away, looking almost along the root's side direction (10 degrees off it).
    const float off = 10.0f * 3.14159265f / 180.0f;
    const vec3 toCamera = Shader::normalize(kSideDir * std::cos(off) + kBendDir * std::sin(off));
    Shader::GrassBladeFloor floor = NoFloor();
    floor.CameraPos = root + toCamera * 10.0f + kUp * 0.36f;
    floor.MinHalfWidth = 3.0f * kHalfWidth; // the root asks for a 3x widening
    const float twist = 0.5f * 3.14159265f;
    float rootProjected = 0.0f;
    for (int step = 0; step <= 10; ++step)
    {
        const float t = static_cast<float>(step) / 10.0f;
        const auto left = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, 0.0f, 0.0f, twist, t,
                                                        kBladeHeight, -1.0f, kHalfWidth, floor);
        const auto right = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, 0.0f, 0.0f, twist, t,
                                                         kBladeHeight, 1.0f, kHalfWidth, floor);
        const vec3 mid = (left.Position + right.Position) * 0.5f;
        const vec3 view = Shader::normalize(mid - floor.CameraPos);
        const vec3 across = right.Position - left.Position;
        const float projected = Shader::length(Shader::cross(across, view));
        if (step == 0)
            rootProjected = projected;
        // Never wider on screen than the floor's width at the root, or than the blade's own width.
        const float own = 2.0f * kHalfWidth * Shader::length(Shader::cross(Shader::normalize(across), view));
        EXPECT_LE(projected, std::max(rootProjected, own) * 1.05f + 1e-6f) << "t=" << t;
        // And never narrower than the floor or the blade's own width allows.
        EXPECT_GE(projected, std::min(rootProjected, own) * 0.95f - 1e-6f) << "t=" << t;
    }
    EXPECT_GT(rootProjected, 2.0f * kHalfWidth * std::sin(off) * 2.0f) << "the floor widens the edge-on root";
}

// The facing ratio stays finite whatever the view vector: zero length (the camera on the vertex),
// underflowing, or along the root's side (the root edge-on).
TEST(GrassBladeVertex, TheFacingRatioIsFiniteForAnyViewVector)
{
    const vec3 twisted = Shader::normalize(kSideDir + kBendDir);
    for (const vec3& view : {vec3(0.0f, 0.0f, 0.0f), vec3(1.0e-30f, 0.0f, 1.0e-30f), kSideDir * 1000.0f,
                             kBendDir * 5.0f})
    {
        const float facing = Shader::GrassBladeFacingRatio(kSideDir, twisted, view);
        EXPECT_TRUE(std::isfinite(facing)) << "view=(" << view.x << "," << view.y << "," << view.z << ")";
        EXPECT_GE(facing, 0.0f);
    }
}

// The fold fades out as the leaf's projected width falls toward a few pixels: at one pixel the two
// edge normals are the face normal.
TEST(GrassBladeVertex, TheFoldFadesOutAsTheLeafNarrowsToAFewPixels)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    Shader::GrassBladeFloor floor = NoFloor();
    floor.CameraPos = root - Shader::cross(kSideDir, kUp) * 20.0f;
    floor.RootPixels = 1.0f;
    const auto left = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, kTilt, kBend, 0.0f, 0.3f,
                                                    kBladeHeight, -1.0f, kHalfWidth, floor);
    const auto right = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, kTilt, kBend, 0.0f, 0.3f,
                                                     kBladeHeight, 1.0f, kHalfWidth, floor);
    EXPECT_NEAR(AngleBetween(left.Normal, right.Normal), 0.0f, 1e-4f);
    floor.RootPixels = 8.0f;
    const auto wideLeft = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, kTilt, kBend, 0.0f, 0.3f,
                                                        kBladeHeight, -1.0f, kHalfWidth, floor);
    const auto wideRight = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, kTilt, kBend, 0.0f, 0.3f,
                                                         kBladeHeight, 1.0f, kHalfWidth, floor);
    EXPECT_NEAR(AngleBetween(wideLeft.Normal, wideRight.Normal), 2.0f * Shader::kGrassBladeFoldRadians, 1e-3f);
}

// The two edges of an upright, untwisted blade at its root, with the given floor.
float RootStripWidth(float halfWidth, const Shader::GrassBladeFloor& floor)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    const auto left = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, 0.0f, 0.0f, 0.0f, 0.0f,
                                                    kBladeHeight, -1.0f, halfWidth, floor);
    const auto right = Shader::GrassBuildBladeVertex(root, kUp, kBendDir, kSideDir, 0.0f, 0.0f, 0.0f, 0.0f,
                                                     kBladeHeight, 1.0f, halfWidth, floor);
    return Shader::length(right.Position - left.Position);
}

// BladeWidth is the whole leaf: the strip at the root is exactly that wide, and never wider than a
// fifth of the blade's height.
TEST(GrassBladeVertex, TheStripIsTheWholeBladeWidthWide)
{
    constexpr float kTaperStart = 0.6f;
    for (float bladeWidth : {0.004f, 0.036f, 0.1f})
    {
        const float half = Shader::GrassBladeHalfWidth(bladeWidth, kBladeHeight, kTaperStart, 0.0f);
        EXPECT_NEAR(RootStripWidth(half, NoFloor()), bladeWidth, 1e-6f) << "BladeWidth=" << bladeWidth;
    }
    const float capped = Shader::GrassBladeHalfWidth(0.5f, kBladeHeight, kTaperStart, 0.0f);
    EXPECT_NEAR(RootStripWidth(capped, NoFloor()), 0.2f * kBladeHeight, 1e-6f) << "the aspect cap";
}

// The pixel floor where the facing does not change along the blade: inert, bit for bit, at or below
// the blade's own width; bound to the floor above it; and never past MaxExpansion times the width.
TEST(GrassBladeVertex, TheFloorIsInertUnderTheWidthBindsAboveItAndStopsAtTheCeiling)
{
    Shader::GrassBladeFloor floor = NoFloor();
    floor.CameraPos = vec3(0.0f, 0.36f, 0.0f) - Shader::cross(kSideDir, kUp) * 10.0f;
    for (float halfWidth : {0.0f, 1.0e-6f, 0.0123f, 0.018f, 0.05f, 1.7f})
    {
        const float own = RootStripWidth(halfWidth, NoFloor());
        for (float minHalfWidth : {0.0f, 0.5f * halfWidth, halfWidth})
        {
            floor.MinHalfWidth = minHalfWidth;
            EXPECT_EQ(RootStripWidth(halfWidth, floor), own)
                << "halfWidth=" << halfWidth << " floor=" << minHalfWidth;
        }
    }
    floor.MinHalfWidth = 0.02f;
    EXPECT_NEAR(RootStripWidth(0.01f, floor), 2.0f * 0.02f, 1e-6f) << "the floor binds";
    floor.MinHalfWidth = 1.0f;
    EXPECT_NEAR(RootStripWidth(0.01f, floor), 2.0f * 0.01f * floor.MaxExpansion, 1e-6f) << "the ceiling";
}

// Parallel sides up to the taper start, then a smooth, monotone taper to a point at the tip.
TEST(GrassBladeVertex, TheWidthIsParallelToTheTaperStartThenTapersToAPoint)
{
    constexpr float kTaperStart = 0.6f;
    float previous = 1.0f;
    for (int step = 0; step <= 100; ++step)
    {
        const float t = static_cast<float>(step) / 100.0f;
        const float w = Shader::GrassBladeWidthProfile(kTaperStart, t);
        if (t <= kTaperStart)
            EXPECT_EQ(w, 1.0f) << "t=" << t;
        EXPECT_LE(w, previous) << "the width grows again at t=" << t;
        previous = w;
    }
    EXPECT_EQ(Shader::GrassBladeWidthProfile(kTaperStart, 1.0f), 0.0f) << "the tip is a point";
}

// The shading normal is the face normal turned about the spine, one way at each edge: both edges
// perpendicular to the spine, mirrored about the face.
TEST(GrassBladeVertex, TheFoldTurnsTheTwoEdgeNormalsAboutTheSpine)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    const float kFold = Shader::kGrassBladeFoldRadians;
    for (float t : {0.0f, 0.3f, 0.8f})
    {
        const Shader::GrassBladeVertex left = BuildVertex(root, t, -1.0f, kTilt, kBend, 1.2f);
        const Shader::GrassBladeVertex right = BuildVertex(root, t, 1.0f, kTilt, kBend, 1.2f);
        const vec3 across = Shader::normalize(right.Position - left.Position);
        const vec3 face = Shader::cross(across, left.Tangent);
        EXPECT_NEAR(AngleBetween(left.Normal, right.Normal), 2.0f * kFold, 1e-4f) << "t=" << t;
        EXPECT_NEAR(AngleBetween(left.Normal, face), kFold, 1e-4f) << "t=" << t;
        EXPECT_NEAR(AngleBetween(right.Normal, face), kFold, 1e-4f) << "t=" << t;
        EXPECT_NEAR(Shader::dot(left.Normal, left.Tangent), 0.0f, 1e-5f) << "t=" << t;
        EXPECT_NEAR(Shader::dot(right.Normal, right.Tangent), 0.0f, 1e-5f) << "t=" << t;
        // The left edge (-1) turns toward the right edge, and the right edge toward the left.
        EXPECT_GT(Shader::dot(left.Normal, across), 0.0f) << "t=" << t;
        EXPECT_LT(Shader::dot(right.Normal, across), 0.0f) << "t=" << t;
    }
}

// THE SHADING NORMAL RULE: a blade shades with its own face normal, turned at most 15 degrees about
// the spine at each edge, so the leaf reads as a shallow rounded channel. A wider turn splits every
// blade into a sunlit half and a dark half, and the twist hands the camera the dark half somewhere
// along the blade, which reads as a tip darker than its base (#4346).
TEST(GrassBladeVertex, TheLeafShadesAsAShallowRoundedChannel)
{
    constexpr float kMostTurn = 15.0f * 3.14159265f / 180.0f;
    const vec3 root(0.0f, 0.0f, 0.0f);
    for (float t : {0.0f, 0.5f, 1.0f})
    {
        const Shader::GrassBladeVertex left = BuildVertex(root, t, -1.0f, kTilt, kBend, 1.2f);
        const Shader::GrassBladeVertex right = BuildVertex(root, t, 1.0f, kTilt, kBend, 1.2f);
        EXPECT_LE(AngleBetween(left.Normal, right.Normal), 2.0f * kMostTurn + 1e-4f) << "t=" << t;
    }
}

// The rows of the blade strip: every straight segment between two rows strays from the curved spine
// by about the same amount, on the shipped angle law with its tip droop, for a calm blade and a
// strongly bent one. Rows that ignore the droop leave the segments near the tip half as far again
// from the spine as those near the root, which reads as knees where the eye looks.
float SegmentStray(float t0, float t1, float bend)
{
    const vec3 root(0.0f, 0.0f, 0.0f);
    const vec3 a = BuildVertex(root, t0, 0.0f, kTilt, bend).Position;
    const vec3 b = BuildVertex(root, t1, 0.0f, kTilt, bend).Position;
    const vec3 chord = Shader::normalize(b - a);
    float stray = 0.0f;
    for (int i = 1; i < 64; ++i)
    {
        const float t = t0 + (t1 - t0) * static_cast<float>(i) / 64.0f;
        const vec3 offset = BuildVertex(root, t, 0.0f, kTilt, bend).Position - a;
        stray = std::max(stray, Shader::length(offset - chord * Shader::dot(offset, chord)));
    }
    return stray;
}

TEST(GrassBladeVertex, TheRowsSpreadTheBendEvenlyAlongTheBlade)
{
    EXPECT_FLOAT_EQ(GameEngine::TerrainGrass::kGrassBladeDroopRatio, Shader::kGrassBladeDroopRatio)
        << "the rows are spaced for a different angle law than the shader bends";
    for (uint32_t segments : {5u, 8u})
    {
        EXPECT_FLOAT_EQ(GameEngine::TerrainGrass::GrassBladeRowHeight(0u, segments), 0.0f);
        EXPECT_FLOAT_EQ(GameEngine::TerrainGrass::GrassBladeRowHeight(segments, segments), 1.0f);
        for (float bend : {0.35f, 1.15f})
        {
            float least = 1e9f;
            float most = 0.0f;
            for (uint32_t row = 0; row < segments; ++row)
            {
                const float t0 = GameEngine::TerrainGrass::GrassBladeRowHeight(row, segments);
                const float t1 = GameEngine::TerrainGrass::GrassBladeRowHeight(row + 1u, segments);
                ASSERT_GT(t1, t0) << "rows " << row << " and " << row + 1u;
                const float stray = SegmentStray(t0, t1, bend);
                least = std::min(least, stray);
                most = std::max(most, stray);
            }
            EXPECT_LT(most / least, 1.35f) << segments << " segments, bend " << bend;
        }
    }
}

// The block above is only half of the property: the vertex stage must also hand the block's sample
// point, and nothing else, to the one root resolve, through a FLAT varying, so every fragment of a
// blade reads the colour its vertices resolved at the root.
TEST(GrassBladeVertex, TheVertexStageResolvesTheGroundColourAtTheBlocksSamplePoint)
{
    std::ifstream file(std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) + "/terrain_grass_vertex_modifier.glsl");
    ASSERT_TRUE(file.good()) << "cannot read terrain_grass_vertex_modifier.glsl";
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();

    // Both point arguments: the world point the splat and the untextured variation read, and the
    // render-origin-relative point the triplanar projection UVs read. Either one following the
    // vertex brings the ground's pattern back up the blade.
    const std::regex call(R"(GrassRootGroundColor\(\s*tp\s*,\s*([^,]+),([^;]+)\)\s*\)\s*;)");
    std::vector<std::pair<std::string, std::string>> arguments;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), call); it != std::sregex_iterator(); ++it)
        arguments.emplace_back((*it)[1].str(), (*it)[2].str());
    ASSERT_EQ(arguments.size(), 1u) << "expected exactly one root resolve call in the vertex stage";
    const std::regex space(R"(\s+)");
    EXPECT_EQ(std::regex_replace(arguments[0].first, space, ""), "bladeVertex.GroundSamplePoint")
        << "the root resolve's world point is not the blade-vertex block's sample point";
    EXPECT_EQ(std::regex_replace(arguments[0].second, space, ""),
              "bladeVertex.GroundSamplePoint-GE_RenderOriginWorld()")
        << "the root resolve's render-origin-relative point is not the block's sample point";
    EXPECT_TRUE(std::regex_search(source, std::regex(R"(flat\s+out\s+vec4\s+vGrassBlade\s*;)")))
        << "the root ground colour no longer reaches the surface through a flat varying";
}
