// The planar split demand (cbt_planar_demand.glsl): a child never demands more than its parent, and the
// height term r bounds the surface's distance from the facet's own plane.
//
// The shared block of cbt_planar_demand.glsl is extracted at build time (Tests/ExtractShaderBlock.cmake)
// and compiled here through GlslShim.h, so the oracle runs the shader's own demand, depth bound, flat
// veto, plane fold and curvature bound. It walks random LEB paths from the two roots and asks each
// parent and child the same question under random cameras: eye height looking along the ground, a
// grazing view across ridges at the lattice's frequency, views from above and views at the screen's
// edges. A node's box is its UV box in XZ with the range of a min/max block pyramid's cover in Y, built
// and read as the kernels build and read theirs (aligned blocks nest, so the cover of a child lies in
// the cover of its parent). r is the smallest of the box's range, the step bound, the curvature bound
// and the plane width, as CBT_PlanarNodeBox and CBT_PlanarNodeSplits take it, with the first stored
// level at 2-cell blocks (a 1025^2 lattice) and at 8-cell blocks (a 4097^2 lattice).
//
//   AChildNeverDemandsMoreThanItsParent - over 60,000 parent and child pairs on each terrain and first
//     level, the child's r and demand are at most its parent's, a parent the flat veto holds holds its
//     child, and the surface at points inside each facet (lattice samples, lattice-line crossings of
//     its edges, the peak in each cell along a diagonal edge, random points) stands at most r from the plane through its corners. On a tilted plane
//     the flat veto holds every facet. The positive control: the projected split edge on the
//     point-sampled corners has a child above its parent on the same pairs.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace GameEngine::GlslShim::Shader
{
#define precise
#include "CBTPlanarDemandExtracted.h"
#undef precise
} // namespace GameEngine::GlslShim::Shader

#include "CBTReliefHeightSource.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"

namespace
{
using namespace GameEngine::GlslShim;
using GameEngine::CBTTerrain::Test::ReliefHeightAt;

constexpr float kScreenW = 3067.0f;
constexpr float kScreenH = 900.0f;
constexpr float kFovY = 1.0471976f; // 60 degrees, the editor's default
constexpr float kMargin = 1.1f;     // cbt_kernels.comp CBT_SPLIT_NDC_MARGIN
constexpr float kFlatPx = 1.0f;     // the shipped flat veto
constexpr uint32_t kSamples = 1025u;
constexpr uint32_t kMaxDepth = 21u;
constexpr uint32_t kPairs = 60000u;
constexpr uint32_t kRandomPoints = 16u;
// Boxes up to this many cells across are also checked at every lattice sample inside the facet, at
// every crossing of a lattice line with its edges, where the bilinear surface bends, and at the peak in
// each cell along a diagonal edge.
constexpr float kLatticeCheckCells = 8.0f;
constexpr float kNoBound = 3.0e38f;

constexpr uint32_t kCells = kSamples - 1u;
constexpr uint32_t kTopLevel = 10u; // 2^10 = kCells

enum class TerrainKind : uint32_t
{
    Ridges,
    Relief,
    Cliff,
    TiltedPlane,
};

// A heightfield on a square terrain: the lattice, its size and relief in meters, and its block pyramids
// from the first stored level `First` up (level k: blocks of 2^k cells): the min/max range of the samples
// each block covers, its boundary included; each block's plane (CBTLayout.h kCBTHeightPlaneFirstWord);
// and the first level's differences (kCBTHeightDifferencesFirstWord).
struct Terrain
{
    const char* Label = "";
    TerrainKind Kind = TerrainKind::Ridges;
    uint32_t First = 1u;
    float SizeM = 0.0f;
    float HeightM = 0.0f;
    std::vector<float> Samples;            // kSamples^2, normalized
    std::vector<std::vector<vec2>> Ranges; // [level][block] low, high
    std::vector<std::vector<vec4>> Planes; // [level][block] slope x, slope z, residual low, width
    std::vector<vec4> Differences;         // [block] of the first level
};

float At(const Terrain& terrain, uint32_t x, uint32_t z)
{
    return terrain.Samples[static_cast<size_t>(z) * kSamples + x];
}

// The first level's block (x0, z0) - (x1, z1) in samples, as cbt_kernels.comp CBT_SampleDifferences
// reads sample (x, z).
struct SampleBlock
{
    uint32_t X0, Z0, X1, Z1;
};

vec4 SampleDifferences(const Terrain& terrain, uint32_t x, uint32_t z, const SampleBlock& b)
{
    const float h = At(terrain, x, z);
    vec4 d(0.0f);
    if (x > b.X0)
        d.x = std::abs(h - At(terrain, x - 1u, z));
    if (z > b.Z0)
        d.x = std::max(d.x, std::abs(h - At(terrain, x, z - 1u)));
    if (x > 0u && x < kCells)
        d.y = std::abs(At(terrain, x - 1u, z) - 2.0f * h + At(terrain, x + 1u, z));
    if (z > 0u && z < kCells)
        d.z = std::abs(At(terrain, x, z - 1u) - 2.0f * h + At(terrain, x, z + 1u));
    if (x < b.X1 && z < b.Z1)
        d.w = std::abs(At(terrain, x + 1u, z + 1u) - At(terrain, x + 1u, z) - At(terrain, x, z + 1u) + h);
    return d;
}

// cbt_kernels.comp Kernel_HeightRangeBuild at the first stored level: the range, the differences and the
// least-squares plane of block (bx, bz).
void BuildFirstLevelBlock(Terrain& terrain, uint32_t bx, uint32_t bz)
{
    const uint32_t level = terrain.First;
    const SampleBlock b{bx << level, bz << level, std::min((bx + 1u) << level, kCells),
                        std::min((bz + 1u) << level, kCells)};
    const float blockCells = static_cast<float>(1u << level);
    const vec2 center((static_cast<float>(bx) + 0.5f) * blockCells, (static_cast<float>(bz) + 0.5f) * blockCells);
    const vec2 mean(0.5f * static_cast<float>(b.X0 + b.X1), 0.5f * static_cast<float>(b.Z0 + b.Z1));
    const float origin = At(terrain, b.X0, b.Z0);
    vec2 range(kNoBound, -kNoBound);
    vec4 differences(0.0f);
    vec4 moments(0.0f); // sum of dx h, dx^2, dz h, dz^2
    for (uint32_t z = b.Z0; z <= b.Z1; ++z)
        for (uint32_t x = b.X0; x <= b.X1; ++x)
        {
            const float h = At(terrain, x, z);
            range = vec2(std::min(range.x, h), std::max(range.y, h));
            differences = max(differences, SampleDifferences(terrain, x, z, b));
            const vec2 d = vec2(static_cast<float>(x), static_cast<float>(z)) - mean;
            const float relative = h - origin;
            moments = vec4(moments.x + d.x * relative, moments.y + d.x * d.x, moments.z + d.y * relative,
                           moments.w + d.y * d.y);
        }
    const vec2 slope(moments.x / moments.y, moments.z / moments.w);
    vec2 residual(kNoBound, -kNoBound);
    for (uint32_t z = b.Z0; z <= b.Z1; ++z)
        for (uint32_t x = b.X0; x <= b.X1; ++x)
        {
            const vec2 d = vec2(static_cast<float>(x), static_cast<float>(z)) - center;
            const float r = (At(terrain, x, z) - origin) - slope.x * d.x - slope.y * d.y;
            residual = vec2(std::min(residual.x, r), std::max(residual.y, r));
        }
    const size_t e = static_cast<size_t>(bz) * (kCells >> level) + bx;
    terrain.Ranges[level][e] = range;
    terrain.Differences[e] = differences;
    terrain.Planes[level][e] = vec4(slope.x, slope.y, residual.x + origin, residual.y - residual.x);
}

// cbt_kernels.comp Kernel_HeightRangeBuild above the first stored level: the union of the 2 x 2
// children's ranges, and their planes folded (CBT_FoldHeightPlane).
void BuildFoldedBlock(Terrain& terrain, uint32_t level, uint32_t bx, uint32_t bz)
{
    const uint32_t below = kCells >> (level - 1u);
    const float childCells = static_cast<float>(1u << (level - 1u));
    const vec2 center((static_cast<float>(bx) + 0.5f) * 2.0f * childCells,
                      (static_cast<float>(bz) + 0.5f) * 2.0f * childCells);
    vec2 range(kNoBound, -kNoBound);
    std::array<vec4, 4> child;
    std::array<vec2, 4> offset;
    for (uint32_t i = 0; i < 4u; ++i)
    {
        const uint32_t cx = bx * 2u + (i & 1u);
        const uint32_t cz = bz * 2u + (i >> 1u);
        const size_t c = static_cast<size_t>(cz) * below + cx;
        range = vec2(std::min(range.x, terrain.Ranges[level - 1u][c].x), std::max(range.y, terrain.Ranges[level - 1u][c].y));
        child[i] = terrain.Planes[level - 1u][c];
        offset[i] = vec2((static_cast<float>(cx) + 0.5f) * childCells, (static_cast<float>(cz) + 0.5f) * childCells) - center;
    }
    const size_t e = static_cast<size_t>(bz) * (kCells >> level) + bx;
    terrain.Ranges[level][e] = range;
    terrain.Planes[level][e] = Shader::CBT_PlaneFold(child[0], child[1], child[2], child[3], offset[0], offset[1],
                                               offset[2], offset[3], 0.5f * childCells);
}

void BuildPyramid(Terrain& terrain)
{
    terrain.Ranges.assign(kTopLevel + 1u, {});
    terrain.Planes.assign(kTopLevel + 1u, {});
    terrain.Differences.assign(static_cast<size_t>(kCells >> terrain.First) * (kCells >> terrain.First), vec4(0.0f));
    for (uint32_t level = terrain.First; level <= kTopLevel; ++level)
    {
        const uint32_t dim = kCells >> level;
        terrain.Ranges[level].resize(static_cast<size_t>(dim) * dim);
        terrain.Planes[level].resize(static_cast<size_t>(dim) * dim);
        for (uint32_t bz = 0; bz < dim; ++bz)
            for (uint32_t bx = 0; bx < dim; ++bx)
            {
                if (level == terrain.First)
                    BuildFirstLevelBlock(terrain, bx, bz);
                else
                    BuildFoldedBlock(terrain, level, bx, bz);
            }
    }
}

float KindHeight(TerrainKind kind, uint32_t x, uint32_t z)
{
    const float u = static_cast<float>(x) / static_cast<float>(kCells);
    const float v = static_cast<float>(z) / static_cast<float>(kCells);
    switch (kind)
    {
    case TerrainKind::Ridges:
        return (x % 2u == 0u) ? 1.0f : 0.0f;
    case TerrainKind::Relief:
        return ReliefHeightAt(u, v);
    case TerrainKind::Cliff:
        return GameEngine::Math::Clamp01((u - 0.5f) * 4096.0f);
    case TerrainKind::TiltedPlane:
        return 0.3f + 0.4f * u + 0.2f * v;
    }
    return 0.0f;
}

// Ridges at the lattice's own frequency along X (the R3 terrain: 1 on even columns, 0 on odd), a smooth
// six-octave relief, a cliff (a 300 m step with a 1 m ramp, where the depth cap never binds), and the
// 4 km preset's square tilted along both axes, with no relief at all.
Terrain MakeTerrain(TerrainKind kind, uint32_t first)
{
    Terrain terrain{};
    terrain.Kind = kind;
    terrain.First = first;
    terrain.Samples.resize(static_cast<size_t>(kSamples) * kSamples);
    for (uint32_t z = 0; z < kSamples; ++z)
        for (uint32_t x = 0; x < kSamples; ++x)
            terrain.Samples[static_cast<size_t>(z) * kSamples + x] = KindHeight(kind, x, z);
    switch (kind)
    {
    case TerrainKind::Ridges:
        terrain.Label = "ridges";
        terrain.SizeM = 512.0f;
        terrain.HeightM = 1.0f;
        break;
    case TerrainKind::Relief:
        terrain.Label = "relief";
        terrain.SizeM = 512.0f;
        terrain.HeightM = 60.0f;
        break;
    case TerrainKind::Cliff:
        terrain.Label = "cliff";
        terrain.SizeM = 4096.0f;
        terrain.HeightM = 300.0f;
        break;
    case TerrainKind::TiltedPlane:
        terrain.Label = "tilted plane";
        terrain.SizeM = 4096.0f;
        terrain.HeightM = 512.0f;
        break;
    }
    BuildPyramid(terrain);
    return terrain;
}

// The lattice cells [X0, X1] x [Z0, Z1] a UV box touches (cbt_kernels.comp CBT_RangeCells).
struct CellSpan
{
    uint32_t X0, Z0, X1, Z1;
};

CellSpan RangeCells(vec2 lo, vec2 hi)
{
    const float cf = static_cast<float>(kCells);
    const auto low = [cf](float uv) { return std::clamp(std::floor(GameEngine::Math::Clamp01(uv) * cf), 0.0f, cf - 1.0f); };
    const float x0 = low(lo.x), z0 = low(lo.y);
    const float x1 = std::clamp(std::ceil(GameEngine::Math::Clamp01(hi.x) * cf) - 1.0f, x0, cf - 1.0f);
    const float z1 = std::clamp(std::ceil(GameEngine::Math::Clamp01(hi.y) * cf) - 1.0f, z0, cf - 1.0f);
    return {static_cast<uint32_t>(x0), static_cast<uint32_t>(z0), static_cast<uint32_t>(x1), static_cast<uint32_t>(z1)};
}

// The pyramid's cover of a UV box (cbt_kernels.comp CBT_PyramidRangeOverUV): the lowest and highest
// normalized height over at most 2 x 2 blocks of the coarsest level that holds the cells to two blocks
// per axis, never finer than the first stored level, and the largest of each difference in those
// blocks when they are the first level's, else kNoBound.
struct Cover
{
    vec2 Range;
    vec4 Differences;
};

Cover PyramidCover(const Terrain& terrain, vec2 lo, vec2 hi)
{
    const CellSpan s = RangeCells(lo, hi);
    const uint32_t span = std::max(s.X1 - s.X0, s.Z1 - s.Z0);
    const uint32_t level =
        std::clamp(span <= 1u ? 0u : static_cast<uint32_t>(std::bit_width(span - 1u)), terrain.First, kTopLevel);
    const uint32_t dim = kCells >> level;
    Cover c{vec2(kNoBound, -kNoBound), vec4(level == terrain.First ? 0.0f : kNoBound)};
    for (uint32_t bz = s.Z0 >> level; bz <= (s.Z1 >> level); ++bz)
        for (uint32_t bx = s.X0 >> level; bx <= (s.X1 >> level); ++bx)
        {
            const vec2& r = terrain.Ranges[level][static_cast<size_t>(bz) * dim + bx];
            c.Range = vec2(std::min(c.Range.x, r.x), std::max(c.Range.y, r.y));
            if (level == terrain.First)
                c.Differences = max(c.Differences, terrain.Differences[static_cast<size_t>(bz) * dim + bx]);
        }
    return c;
}

// The plane width of the smallest stored block that contains the cells a UV box touches
// (cbt_kernels.comp CBT_PlaneWidthOverUV), normalized.
float PlaneWidth(const Terrain& terrain, vec2 lo, vec2 hi)
{
    const CellSpan s = RangeCells(lo, hi);
    const uint32_t differ = (s.X0 ^ s.X1) | (s.Z0 ^ s.Z1);
    const uint32_t level =
        std::clamp(differ == 0u ? 0u : static_cast<uint32_t>(std::bit_width(differ)), terrain.First, kTopLevel);
    return terrain.Planes[level][static_cast<size_t>(s.Z0 >> level) * (kCells >> level) + (s.X0 >> level)].w;
}

// The point-sampled height at a UV, as VertexEval caches a corner (bilinear between lattice samples).
float CornerHeight(const Terrain& terrain, vec2 uv)
{
    const float cf = static_cast<float>(kCells);
    const float fx = GameEngine::Math::Clamp01(uv.x) * cf;
    const float fz = GameEngine::Math::Clamp01(uv.y) * cf;
    const auto x = std::min(static_cast<uint32_t>(fx), kSamples - 2u);
    const auto z = std::min(static_cast<uint32_t>(fz), kSamples - 2u);
    const float ax = fx - static_cast<float>(x);
    const float az = fz - static_cast<float>(z);
    const float h0 = At(terrain, x, z) + (At(terrain, x + 1u, z) - At(terrain, x, z)) * ax;
    const float h1 = At(terrain, x, z + 1u) + (At(terrain, x + 1u, z + 1u) - At(terrain, x, z + 1u)) * ax;
    return (h0 + (h1 - h0) * az) * terrain.HeightM;
}

struct Camera
{
    Shader::CBTPlanarView View;
    vec3 Eye;
    GameEngine::Mathematics::Matrix4x4 ViewProjection;
};

Camera MakeCamera(const Terrain& terrain, std::mt19937& rng)
{
    using namespace GameEngine::Mathematics;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float x = (0.1f + 0.8f * unit(rng)) * terrain.SizeM;
    const float z = (0.1f + 0.8f * unit(rng)) * terrain.SizeM;
    const float pick = unit(rng);
    // Eye height (1.7 m), a low flight and a view from high above, over the ground under the eye.
    const float above = pick < 0.5f ? 1.7f : (pick < 0.8f ? 30.0f : 400.0f);
    const float ground = CornerHeight(terrain, vec2(x / terrain.SizeM, z / terrain.SizeM));
    const float yaw = unit(rng) * 6.2831853f;
    const float pitch = -0.6f * unit(rng) + 0.05f;
    const Vector3 eye(x, ground + above, z);
    const Vector3 dir(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kScreenW / kScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + dir, Vector3(0, 1, 0));
    Camera c{};
    c.ViewProjection = vp;
    // Rows of the column-major matrix, as CBT_MakePlanarCriterion takes them.
    const auto row = [&](int r) { return vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]); };
    c.View.RowX = row(0);
    c.View.RowY = row(1);
    c.View.RowW = row(3);
    const float scaleX = std::sqrt(c.View.RowX.x * c.View.RowX.x + c.View.RowX.y * c.View.RowX.y + c.View.RowX.z * c.View.RowX.z);
    const float scaleY = std::sqrt(c.View.RowY.x * c.View.RowY.x + c.View.RowY.y * c.View.RowY.y + c.View.RowY.z * c.View.RowY.z);
    c.View.FocalPx = scaleY * 0.5f * kScreenH;
    c.View.ViewPerClipX = 1.0f / scaleX;
    c.View.ViewPerClipY = 1.0f / scaleY;
    c.View.Kappa = 1.0f / std::sqrt(1.0f + c.View.ViewPerClipX * c.View.ViewPerClipX + c.View.ViewPerClipY * c.View.ViewPerClipY);
    c.View.MarginTanX = kMargin * c.View.ViewPerClipX;
    c.View.MarginTanY = kMargin * c.View.ViewPerClipY;
    c.View.Margin = kMargin;
    c.View.Ortho = 0u;
    c.Eye = vec3(eye.x, eye.y, eye.z);
    return c;
}

using Facet = std::array<vec2, 3>;

vec2 Midpoint(vec2 a, vec2 b)
{
    return vec2(0.5f * (a.x + b.x), 0.5f * (a.y + b.y));
}

// One random LEB path from a root to `depth`: the facet at `depth - 1` and its child at `depth`.
std::array<Facet, 2> RandomPair(uint32_t depth, std::mt19937& rng)
{
    Facet f = (rng() & 1u) == 0u ? Facet{vec2(1.0f, 0.0f), vec2(0.0f, 0.0f), vec2(0.0f, 1.0f)}
                                 : Facet{vec2(0.0f, 1.0f), vec2(1.0f, 1.0f), vec2(1.0f, 0.0f)};
    Facet parent = f;
    for (uint32_t d = 1u; d < depth; ++d)
    {
        parent = f;
        const vec2 mid = Midpoint(f[0], f[2]);
        f = (rng() & 1u) == 0u ? Facet{f[2], mid, f[1]} : Facet{f[1], mid, f[0]};
    }
    return {parent, f};
}

// The barycentric weights of `p` in facet `f`.
vec3 Barycentric(const Facet& f, vec2 p)
{
    const vec2 e1 = f[1] - f[0];
    const vec2 e2 = f[2] - f[0];
    const vec2 q = p - f[0];
    const float det = e1.x * e2.y - e1.y * e2.x;
    const float b1 = (q.x * e2.y - q.y * e2.x) / det;
    const float b2 = (e1.x * q.y - e1.y * q.x) / det;
    return vec3(1.0f - b1 - b2, b1, b2);
}

// The distance between the bilinear surface and the plane through the facet's corner heights at `p`
// (meters), 0 outside the facet.
float PointDeviationM(const Terrain& terrain, const Facet& f, const std::array<float, 3>& cornerM, vec2 p)
{
    const vec3 b = Barycentric(f, p);
    if (b.x < -1.0e-5f || b.y < -1.0e-5f || b.z < -1.0e-5f)
        return 0.0f;
    return std::abs(CornerHeight(terrain, p) - (b.x * cornerM[0] + b.y * cornerM[1] + b.z * cornerM[2]));
}

// The deviation at the peak of a diagonal edge's run [s0, s1] through one cell, s along the edge from
// corner `edge` to the next. Along a line that is not axis-aligned the bilinear surface is quadratic and
// the plane is linear, so the signed distance is a parabola whose vertex, found from three evenly spaced
// samples, can lie between the lattice crossings.
float EdgeCellPeakDeviationM(const Terrain& terrain, const Facet& f, const std::array<float, 3>& cornerM, uint32_t edge, float s0, float s1)
{
    constexpr float kMinRun = 1.0e-6f;
    if (s1 - s0 < kMinRun)
        return 0.0f;
    const vec2 a = f[edge];
    const vec2 b = f[(edge + 1u) % 3u];
    const float heightA = cornerM[edge];
    const float heightB = cornerM[(edge + 1u) % 3u];
    const auto signedDistance = [&](float s) { return CornerHeight(terrain, a + (b - a) * s) - (heightA + (heightB - heightA) * s); };
    const float half = 0.5f * (s1 - s0);
    const float mid = s0 + half;
    const float y0 = signedDistance(s0);
    const float ym = signedDistance(mid);
    const float y1 = signedDistance(s1);
    const float curvature = y0 - 2.0f * ym + y1;
    if (curvature == 0.0f)
        return 0.0f;
    const float peak = std::clamp(mid + half * (y0 - y1) / (2.0f * curvature), s0, s1);
    return PointDeviationM(terrain, f, cornerM, a + (b - a) * peak);
}

// The largest distance (meters) between the bilinear surface and the facet's plane over random points
// inside the facet, its edges' midpoints and, for a box at most kLatticeCheckCells across, every lattice
// sample inside it, every crossing of a lattice line with its edges and the peak in each cell along a
// diagonal edge.
float FacetDeviationM(const Terrain& terrain, const Facet& f, vec2 lo, vec2 hi, std::mt19937& rng)
{
    const std::array<float, 3> cornerM = {CornerHeight(terrain, f[0]), CornerHeight(terrain, f[1]), CornerHeight(terrain, f[2])};
    float worst = 0.0f;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (uint32_t i = 0; i < kRandomPoints; ++i)
    {
        float a = unit(rng);
        float b = unit(rng);
        if (a + b > 1.0f)
        {
            a = 1.0f - a;
            b = 1.0f - b;
        }
        worst = std::max(worst, PointDeviationM(terrain, f, cornerM, f[0] + (f[1] - f[0]) * a + (f[2] - f[0]) * b));
    }
    for (uint32_t i = 0; i < 3u; ++i)
        worst = std::max(worst, PointDeviationM(terrain, f, cornerM, Midpoint(f[i], f[(i + 1u) % 3u])));
    const float cf = static_cast<float>(kCells);
    if ((hi.x - lo.x) * cf > kLatticeCheckCells || (hi.y - lo.y) * cf > kLatticeCheckCells)
        return worst;
    const auto x0 = static_cast<uint32_t>(std::ceil(lo.x * cf));
    const auto x1 = static_cast<uint32_t>(std::floor(hi.x * cf));
    const auto z0 = static_cast<uint32_t>(std::ceil(lo.y * cf));
    const auto z1 = static_cast<uint32_t>(std::floor(hi.y * cf));
    for (uint32_t z = z0; z <= z1; ++z)
        for (uint32_t x = x0; x <= x1; ++x)
            worst = std::max(worst, PointDeviationM(terrain, f, cornerM, vec2(static_cast<float>(x) / cf, static_cast<float>(z) / cf)));
    std::vector<float> crossings;
    for (uint32_t i = 0; i < 3u; ++i)
    {
        const vec2 a = f[i];
        const vec2 b = f[(i + 1u) % 3u];
        crossings.assign({0.0f, 1.0f});
        for (uint32_t x = x0; x <= x1 && b.x != a.x; ++x)
        {
            const float s = (static_cast<float>(x) / cf - a.x) / (b.x - a.x);
            if (s >= 0.0f && s <= 1.0f)
                crossings.push_back(s);
        }
        for (uint32_t z = z0; z <= z1 && b.y != a.y; ++z)
        {
            const float s = (static_cast<float>(z) / cf - a.y) / (b.y - a.y);
            if (s >= 0.0f && s <= 1.0f)
                crossings.push_back(s);
        }
        std::sort(crossings.begin(), crossings.end());
        for (const float s : crossings)
            worst = std::max(worst, PointDeviationM(terrain, f, cornerM, a + (b - a) * s));
        if (b.x == a.x || b.y == a.y)
            continue;
        for (size_t k = 1; k < crossings.size(); ++k)
            worst = std::max(worst, EdgeCellPeakDeviationM(terrain, f, cornerM, i, crossings[k - 1u], crossings[k]));
    }
    return worst;
}

// What the oracle asks of a node: its demand, whether the flat veto holds it, and r.
struct Answer
{
    float Demand = 0.0f;
    bool Flat = false;
    float EdgePx = 0.0f; // the positive control: the projected split edge on the point-sampled corners
    bool EdgeIn = false;
    float RangeM = 0.0f;     // r, the height term the demand reads
    float DeviationM = 0.0f; // the surface's largest sampled distance from the facet's plane, which r bounds
};

bool ProjectPixels(const Camera& c, const vec3& p, float& px, float& py)
{
    const GameEngine::Mathematics::Matrix4x4& m = c.ViewProjection;
    const float x = m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0];
    const float y = m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1];
    const float w = m[0][3] * p.x + m[1][3] * p.y + m[2][3] * p.z + m[3][3];
    if (w <= 1e-5f)
        return false;
    px = (x / w * 0.5f + 0.5f) * kScreenW;
    py = (y / w * 0.5f + 0.5f) * kScreenH;
    return true;
}

// r as cbt_kernels.comp CBT_PlanarNodeBox and CBT_PlanarNodeSplits take it, without chain levels.
float NodeRangeM(const Terrain& terrain, const Cover& cover, vec2 lo, vec2 hi, float boxHeightM)
{
    const float cf = static_cast<float>(kCells);
    const vec2 cellMin(std::max(lo.x, 0.0f) * cf, std::max(lo.y, 0.0f) * cf);
    const vec2 cellMax(std::min(hi.x, 1.0f) * cf, std::min(hi.y, 1.0f) * cf);
    const float cellsX = std::max(cellMax.x - cellMin.x, 0.0f);
    const float cellsZ = std::max(cellMax.y - cellMin.y, 0.0f);
    float rangeM = Shader::CBT_PlanarStepBoundRange(boxHeightM, cover.Differences.x * terrain.HeightM, cellsX, cellsZ);
    if (cover.Differences.y < kNoBound)
    {
        const float knotsX = std::max(std::ceil(cellMax.x) - std::floor(cellMin.x) - 1.0f, 0.0f);
        const float knotsZ = std::max(std::ceil(cellMax.y) - std::floor(cellMin.y) - 1.0f, 0.0f);
        const float curvatureM =
            Shader::CBT_PlanarCurvatureBound(cover.Differences.y * terrain.HeightM, cover.Differences.z * terrain.HeightM,
                                             cover.Differences.w * terrain.HeightM, cellsX, cellsZ, knotsX, knotsZ);
        rangeM = std::min(rangeM, curvatureM);
    }
    return std::min(rangeM, PlaneWidth(terrain, lo, hi) * terrain.HeightM);
}

Answer Ask(const Terrain& terrain, const Camera& c, const Facet& f, std::mt19937& rng)
{
    const vec2 lo(std::min({f[0].x, f[1].x, f[2].x}), std::min({f[0].y, f[1].y, f[2].y}));
    const vec2 hi(std::max({f[0].x, f[1].x, f[2].x}), std::max({f[0].y, f[1].y, f[2].y}));
    const Cover cover = PyramidCover(terrain, lo, hi);
    const vec3 boxLo(lo.x * terrain.SizeM, cover.Range.x * terrain.HeightM, lo.y * terrain.SizeM);
    const vec3 boxHi(hi.x * terrain.SizeM, cover.Range.y * terrain.HeightM, hi.y * terrain.SizeM);
    const float dist = Shader::CBT_DistanceToBox(boxLo, boxHi, c.Eye);
    const float rangeM = NodeRangeM(terrain, cover, lo, hi, boxHi.y - boxLo.y);
    Answer a;
    a.RangeM = rangeM;
    a.DeviationM = FacetDeviationM(terrain, f, lo, hi, rng);
    a.Flat = Shader::CBT_PlanarBoxFlat(c.View, rangeM, dist, kFlatPx);
    const bool outside = Shader::CBT_BoxOutsideMarginFrustum(c.View, boxLo, boxHi);
    const float depth = Shader::CBT_PlanarBoxDepth(c.View, boxLo, boxHi, dist, outside);
    const float du = f[0].x - f[2].x;
    const float dv = f[0].y - f[2].y;
    const float edgeM = std::sqrt(du * du + dv * dv) * terrain.SizeM;
    a.Demand = Shader::CBT_PlanarBoxDemand(c.View, boxLo, boxHi, depth, outside, Shader::CBT_PlanarNodeSize(edgeM, rangeM));
    const vec3 e0(f[0].x * terrain.SizeM, CornerHeight(terrain, f[0]), f[0].y * terrain.SizeM);
    const vec3 e2(f[2].x * terrain.SizeM, CornerHeight(terrain, f[2]), f[2].y * terrain.SizeM);
    float x0 = 0.0f, y0 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    a.EdgeIn = ProjectPixels(c, e0, x0, y0) && ProjectPixels(c, e2, x2, y2);
    a.EdgePx = std::sqrt((x2 - x0) * (x2 - x0) + (y2 - y0) * (y2 - y0));
    return a;
}

// Whether the sampled distance exceeds r beyond the rounding of fp32 heights in meters.
bool Exceeds(const Terrain& terrain, const Answer& a)
{
    return a.DeviationM > a.RangeM * (1.0f + 1.0e-4f) + 2.0e-6f * terrain.HeightM;
}
} // namespace

TEST(CBTPlanarDemand, AChildNeverDemandsMoreThanItsParent)
{
    for (const uint32_t first : {1u, 3u})
        for (const TerrainKind kind :
             {TerrainKind::Ridges, TerrainKind::Relief, TerrainKind::Cliff, TerrainKind::TiltedPlane})
        {
            const Terrain terrain = MakeTerrain(kind, first);
            SCOPED_TRACE(terrain.Label);
            SCOPED_TRACE(first);
            std::mt19937 rng(1234u + static_cast<uint32_t>(kind) + 16u * first);
            uint32_t above = 0;
            uint32_t rangeAbove = 0;
            uint32_t vetoBroken = 0;
            uint32_t controlAbove = 0;
            uint32_t controlPairs = 0;
            uint32_t vetoed = 0;
            uint32_t unbounded = 0;
            float widestM = 0.0f;
            for (uint32_t i = 0; i < kPairs; ++i)
            {
                const Camera c = MakeCamera(terrain, rng);
                const uint32_t depth = 3u + rng() % (kMaxDepth - 2u);
                const std::array<Facet, 2> pair = RandomPair(depth, rng);
                const Answer parent = Ask(terrain, c, pair[0], rng);
                const Answer child = Ask(terrain, c, pair[1], rng);
                above += child.Demand > parent.Demand ? 1u : 0u;
                rangeAbove += child.RangeM > parent.RangeM ? 1u : 0u;
                vetoBroken += (parent.Flat && !child.Flat) ? 1u : 0u;
                vetoed += parent.Flat ? 1u : 0u;
                unbounded += (Exceeds(terrain, parent) ? 1u : 0u) + (Exceeds(terrain, child) ? 1u : 0u);
                widestM = std::max(widestM, parent.RangeM);
                if (parent.EdgeIn && child.EdgeIn)
                {
                    ++controlPairs;
                    controlAbove += child.EdgePx > parent.EdgePx ? 1u : 0u;
                }
            }
            std::printf("[%s, first level %u] pairs %u: child above parent %u (r %u), veto broken %u (parents "
                        "vetoed %u), facets farther from their plane than r %u, widest r %.3g m; projected edge: "
                        "child above parent %u of %u\n",
                        terrain.Label, first, kPairs, above, rangeAbove, vetoBroken, vetoed, unbounded, widestM, controlAbove,
                        controlPairs);
            if (kind == TerrainKind::TiltedPlane)
                EXPECT_EQ(vetoed, kPairs) << "the flat veto must hold every facet of a plane, whatever its slope";
            else
                EXPECT_GT(controlAbove, controlPairs / 20u) << "positive control: the pairs must expose the projected edge";
            EXPECT_EQ(above, 0u) << "a child demands more than its parent";
            EXPECT_EQ(rangeAbove, 0u) << "a child's r is above its parent's";
            EXPECT_EQ(vetoBroken, 0u) << "the flat veto holds a parent and not its child";
            EXPECT_EQ(unbounded, 0u) << "the surface stands farther from a facet's plane than the r its demand reads";
        }
}
