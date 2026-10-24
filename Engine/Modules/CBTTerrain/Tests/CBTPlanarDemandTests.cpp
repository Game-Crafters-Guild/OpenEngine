// The planar split demand (cbt_planar_demand.glsl): a child never demands more than its parent.
//
// The shared block of cbt_planar_demand.glsl is extracted at build time (Tests/ExtractShaderBlock.cmake)
// and compiled here through GlslShim.h, so the oracle runs the shader's own demand, depth bound and flat
// veto. It walks random LEB paths from the two roots and asks each parent and child the same question
// under random cameras: eye height looking along the ground, a grazing view across ridges at the
// lattice's frequency, views from above and views at the screen's edges. A node's box is its UV box in
// XZ with the range of a min/max block pyramid's cover in Y, built and read as the kernels build and read
// theirs (aligned blocks nest, so the cover of a child lies in the cover of its parent).
//
//   AChildNeverDemandsMoreThanItsParent — over 60,000 parent and child pairs on each terrain, the child's
//     demand is at most its parent's and a parent the flat veto holds holds its child. The positive
//     control: today's planar rule (the projected split edge on the point-sampled corners) has a child
//     above its parent on the same pairs.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t kCells = kSamples - 1u;
constexpr uint32_t kTopLevel = 10u; // 2^10 = kCells

// A heightfield on a square terrain: the lattice, its size and relief in meters, and its min/max block
// pyramid (level k: blocks of 2^k cells, each the range of the samples it covers, its boundary included).
struct Terrain
{
    const char* Label;
    float SizeM;
    float HeightM;
    std::vector<float> Samples;                               // kSamples^2, normalized
    std::vector<std::vector<std::array<float, 2>>> Pyramid; // [level][block], levels 1..kTopLevel
    std::vector<float> Step;                                  // [block] of level 1: the steepest step
};

void BuildPyramid(Terrain& t)
{
    t.Pyramid.resize(kTopLevel + 1u);
    t.Step.assign(static_cast<size_t>(kCells >> 1u) * (kCells >> 1u), 0.0f);
    for (uint32_t level = 1u; level <= kTopLevel; ++level)
    {
        const uint32_t dim = kCells >> level;
        t.Pyramid[level].resize(static_cast<size_t>(dim) * dim);
        for (uint32_t bz = 0; bz < dim; ++bz)
            for (uint32_t bx = 0; bx < dim; ++bx)
            {
                std::array<float, 2> r = {3.0e38f, -3.0e38f};
                if (level == 1u)
                {
                    float step = 0.0f;
                    for (uint32_t z = bz * 2u; z <= bz * 2u + 2u; ++z)
                        for (uint32_t x = bx * 2u; x <= bx * 2u + 2u; ++x)
                        {
                            const float h = t.Samples[static_cast<size_t>(z) * kSamples + x];
                            r = {std::min(r[0], h), std::max(r[1], h)};
                            if (x > bx * 2u)
                                step = std::max(step, std::abs(h - t.Samples[static_cast<size_t>(z) * kSamples + x - 1u]));
                            if (z > bz * 2u)
                                step = std::max(step, std::abs(h - t.Samples[static_cast<size_t>(z - 1u) * kSamples + x]));
                        }
                    t.Step[static_cast<size_t>(bz) * dim + bx] = step;
                }
                else
                {
                    const uint32_t below = kCells >> (level - 1u);
                    for (uint32_t dz = 0; dz < 2u; ++dz)
                        for (uint32_t dx = 0; dx < 2u; ++dx)
                        {
                            const auto& c = t.Pyramid[level - 1u][static_cast<size_t>(bz * 2u + dz) * below + bx * 2u + dx];
                            r = {std::min(r[0], c[0]), std::max(r[1], c[1])};
                        }
                }
                t.Pyramid[level][static_cast<size_t>(bz) * dim + bx] = r;
            }
    }
}

// Ridges at the lattice's own frequency along X (the R3 terrain: 1 on even columns, 0 on odd), a smooth
// six-octave relief, and a cliff: a 300 m step with a 1 m ramp, where the depth cap never binds.
Terrain MakeTerrain(uint32_t kind)
{
    Terrain t{};
    t.Samples.resize(static_cast<size_t>(kSamples) * kSamples);
    for (uint32_t z = 0; z < kSamples; ++z)
        for (uint32_t x = 0; x < kSamples; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(kSamples - 1u);
            const float v = static_cast<float>(z) / static_cast<float>(kSamples - 1u);
            float h = 0.0f;
            if (kind == 0u)
                h = (x % 2u == 0u) ? 1.0f : 0.0f;
            else if (kind == 1u)
                h = ReliefHeightAt(u, v);
            else
                h = GameEngine::Math::Clamp01((u - 0.5f) * 4096.0f);
            t.Samples[static_cast<size_t>(z) * kSamples + x] = h;
        }
    t.Label = kind == 0u ? "ridges" : (kind == 1u ? "relief" : "cliff");
    t.SizeM = kind == 2u ? 4096.0f : 512.0f;
    t.HeightM = kind == 0u ? 1.0f : (kind == 1u ? 60.0f : 300.0f);
    BuildPyramid(t);
    return t;
}

// The lowest and highest normalized height over the lattice cells a UV box touches (cbt_kernels.comp
// CBT_PyramidRangeOverUV): at most 2 x 2 blocks of the coarsest pyramid level that holds the cells to two
// blocks per axis, never finer than level 1 (the first stored level of a 1025^2 lattice). [2] is the
// steepest step in those blocks when they are level 1's, else 3e38.
std::array<float, 3> SampleRange(const Terrain& t, std::array<float, 2> lo, std::array<float, 2> hi)
{
    const float cf = static_cast<float>(kCells);
    const auto c0x = static_cast<uint32_t>(std::clamp(std::floor(lo[0] * cf), 0.0f, cf - 1.0f));
    const auto c0z = static_cast<uint32_t>(std::clamp(std::floor(lo[1] * cf), 0.0f, cf - 1.0f));
    const auto c1x = static_cast<uint32_t>(std::clamp(std::ceil(hi[0] * cf) - 1.0f, static_cast<float>(c0x), cf - 1.0f));
    const auto c1z = static_cast<uint32_t>(std::clamp(std::ceil(hi[1] * cf) - 1.0f, static_cast<float>(c0z), cf - 1.0f));
    const uint32_t span = std::max(c1x - c0x, c1z - c0z);
    std::array<float, 2> range = {3.0e38f, -3.0e38f};
    const uint32_t level = std::clamp(span <= 1u ? 0u : static_cast<uint32_t>(std::bit_width(span - 1u)), 1u, kTopLevel);
    const uint32_t dim = kCells >> level;
    float step = level == 1u ? 0.0f : 3.0e38f;
    for (uint32_t bz = c0z >> level; bz <= (c1z >> level); ++bz)
        for (uint32_t bx = c0x >> level; bx <= (c1x >> level); ++bx)
        {
            const auto& r = t.Pyramid[level][static_cast<size_t>(bz) * dim + bx];
            range = {std::min(range[0], r[0]), std::max(range[1], r[1])};
            if (level == 1u)
                step = std::max(step, t.Step[static_cast<size_t>(bz) * dim + bx]);
        }
    return {range[0], range[1], step};
}

// The point-sampled height at a UV, as VertexEval caches a corner (bilinear between lattice samples).
float CornerHeight(const Terrain& t, std::array<float, 2> uv)
{
    const float cf = static_cast<float>(kSamples - 1u);
    const float fx = GameEngine::Math::Clamp01(uv[0]) * cf;
    const float fz = GameEngine::Math::Clamp01(uv[1]) * cf;
    const auto x = std::min(static_cast<uint32_t>(fx), kSamples - 2u);
    const auto z = std::min(static_cast<uint32_t>(fz), kSamples - 2u);
    const float ax = fx - static_cast<float>(x);
    const float az = fz - static_cast<float>(z);
    const auto at = [&](uint32_t i, uint32_t j) { return t.Samples[static_cast<size_t>(j) * kSamples + i]; };
    const float h0 = at(x, z) + (at(x + 1u, z) - at(x, z)) * ax;
    const float h1 = at(x, z + 1u) + (at(x + 1u, z + 1u) - at(x, z + 1u)) * ax;
    return (h0 + (h1 - h0) * az) * t.HeightM;
}

struct Camera
{
    Shader::CBTPlanarView View;
    vec3 Eye;
    float Vp[16];
};

Camera MakeCamera(const Terrain& t, std::mt19937& rng)
{
    using namespace GameEngine::Mathematics;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float x = (0.1f + 0.8f * unit(rng)) * t.SizeM;
    const float z = (0.1f + 0.8f * unit(rng)) * t.SizeM;
    const float pick = unit(rng);
    // Eye height (1.7 m), a low flight and a view from high above, over the ground under the eye.
    const float above = pick < 0.5f ? 1.7f : (pick < 0.8f ? 30.0f : 400.0f);
    const float ground = CornerHeight(t, {x / t.SizeM, z / t.SizeM});
    const float yaw = unit(rng) * 6.2831853f;
    const float pitch = -0.6f * unit(rng) + 0.05f;
    const Vector3 eye(x, ground + above, z);
    const Vector3 dir(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kFovY, kScreenW / kScreenH, 0.5f, 20000.0f);
    const Matrix4x4 vp = proj * MakeLookAtLH(eye, eye + dir, Vector3(0, 1, 0));
    Camera c{};
    std::memcpy(c.Vp, &vp.GetGLM()[0][0], sizeof(c.Vp));
    // Rows of the column-major matrix, as CBT_MakePlanarCriterion takes them.
    const auto row = [&](uint32_t r) { return vec4(c.Vp[r], c.Vp[4 + r], c.Vp[8 + r], c.Vp[12 + r]); };
    c.View.RowX = row(0u);
    c.View.RowY = row(1u);
    c.View.RowW = row(3u);
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

using Uv = std::array<float, 2>;
using Facet = std::array<Uv, 3>;

// One random LEB path from a root to `depth`: the facet at `depth - 1` and its child at `depth`.
std::array<Facet, 2> RandomPair(uint32_t depth, std::mt19937& rng)
{
    Facet f = (rng() & 1u) == 0u ? Facet{Uv{1.0f, 0.0f}, Uv{0.0f, 0.0f}, Uv{0.0f, 1.0f}}
                                 : Facet{Uv{0.0f, 1.0f}, Uv{1.0f, 1.0f}, Uv{1.0f, 0.0f}};
    Facet parent = f;
    for (uint32_t d = 1u; d < depth; ++d)
    {
        parent = f;
        const Uv mid = {0.5f * (f[0][0] + f[2][0]), 0.5f * (f[0][1] + f[2][1])};
        f = (rng() & 1u) == 0u ? Facet{f[2], mid, f[1]} : Facet{f[1], mid, f[0]};
    }
    return {parent, f};
}

// What the oracle asks of a node: its demand, and whether the flat veto holds it.
struct Answer
{
    float Demand = 0.0f;
    bool Flat = false;
    float EdgePx = 0.0f; // today's rule: the projected split edge on the point-sampled corners
    bool EdgeIn = false;
    float RangeM = 0.0f;        // the range the demand reads
    float CornerSpreadM = 0.0f; // the drawn corners' height spread, which RangeM must bound
};

bool ProjectPixels(const Camera& c, const vec3& p, float& px, float& py)
{
    const float x = c.Vp[0] * p.x + c.Vp[4] * p.y + c.Vp[8] * p.z + c.Vp[12];
    const float y = c.Vp[1] * p.x + c.Vp[5] * p.y + c.Vp[9] * p.z + c.Vp[13];
    const float w = c.Vp[3] * p.x + c.Vp[7] * p.y + c.Vp[11] * p.z + c.Vp[15];
    if (w <= 1e-5f)
        return false;
    px = (x / w * 0.5f + 0.5f) * kScreenW;
    py = (y / w * 0.5f + 0.5f) * kScreenH;
    return true;
}

Answer Ask(const Terrain& t, const Camera& c, const Facet& f)
{
    const Uv lo = {std::min({f[0][0], f[1][0], f[2][0]}), std::min({f[0][1], f[1][1], f[2][1]})};
    const Uv hi = {std::max({f[0][0], f[1][0], f[2][0]}), std::max({f[0][1], f[1][1], f[2][1]})};
    const std::array<float, 3> range = SampleRange(t, lo, hi);
    const vec3 boxLo(lo[0] * t.SizeM, range[0] * t.HeightM, lo[1] * t.SizeM);
    const vec3 boxHi(hi[0] * t.SizeM, range[1] * t.HeightM, hi[1] * t.SizeM);
    const float dist = Shader::CBT_DistanceToBox(boxLo, boxHi, c.Eye);
    const float cellsX = (std::min(hi[0], 1.0f) - std::max(lo[0], 0.0f)) * static_cast<float>(kCells);
    const float cellsZ = (std::min(hi[1], 1.0f) - std::max(lo[1], 0.0f)) * static_cast<float>(kCells);
    const float rangeM = Shader::CBT_PlanarStepBoundRange(boxHi.y - boxLo.y, range[2] * t.HeightM, cellsX, cellsZ);
    Answer a;
    a.RangeM = rangeM;
    const float h0 = CornerHeight(t, f[0]), h1 = CornerHeight(t, f[1]), h2 = CornerHeight(t, f[2]);
    a.CornerSpreadM = std::max({h0, h1, h2}) - std::min({h0, h1, h2});
    a.Flat = Shader::CBT_PlanarBoxFlat(c.View, rangeM, dist, kFlatPx);
    const bool outside = Shader::CBT_BoxOutsideMarginFrustum(c.View, boxLo, boxHi);
    const float depth = Shader::CBT_PlanarBoxDepth(c.View, boxLo, boxHi, dist, outside);
    const float du = f[0][0] - f[2][0];
    const float dv = f[0][1] - f[2][1];
    const float edgeM = std::sqrt(du * du + dv * dv) * t.SizeM;
    a.Demand = Shader::CBT_PlanarBoxDemand(c.View, boxLo, boxHi, depth, outside, Shader::CBT_PlanarNodeSize(edgeM, rangeM));
    const vec3 e0(f[0][0] * t.SizeM, CornerHeight(t, f[0]), f[0][1] * t.SizeM);
    const vec3 e2(f[2][0] * t.SizeM, CornerHeight(t, f[2]), f[2][1] * t.SizeM);
    float x0 = 0.0f, y0 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    a.EdgeIn = ProjectPixels(c, e0, x0, y0) && ProjectPixels(c, e2, x2, y2);
    a.EdgePx = std::sqrt((x2 - x0) * (x2 - x0) + (y2 - y0) * (y2 - y0));
    return a;
}
} // namespace

TEST(CBTPlanarDemand, AChildNeverDemandsMoreThanItsParent)
{
    for (uint32_t kind = 0; kind < 3u; ++kind)
    {
        const Terrain t = MakeTerrain(kind);
        SCOPED_TRACE(t.Label);
        std::mt19937 rng(1234u + kind);
        uint32_t above = 0;
        uint32_t vetoBroken = 0;
        uint32_t todayAbove = 0;
        uint32_t todayPairs = 0;
        uint32_t vetoed = 0;
        uint32_t unbounded = 0;
        for (uint32_t i = 0; i < kPairs; ++i)
        {
            const Camera c = MakeCamera(t, rng);
            const uint32_t depth = 3u + rng() % (kMaxDepth - 2u);
            const std::array<Facet, 2> pair = RandomPair(depth, rng);
            const Answer parent = Ask(t, c, pair[0]);
            const Answer child = Ask(t, c, pair[1]);
            above += child.Demand > parent.Demand ? 1u : 0u;
            vetoBroken += (parent.Flat && !child.Flat) ? 1u : 0u;
            vetoed += parent.Flat ? 1u : 0u;
            unbounded += child.CornerSpreadM > child.RangeM * (1.0f + 1.0e-5f) + 1.0e-6f ? 1u : 0u;
            if (parent.EdgeIn && child.EdgeIn)
            {
                ++todayPairs;
                todayAbove += child.EdgePx > parent.EdgePx ? 1u : 0u;
            }
        }
        std::printf("[%s] pairs %u: child above parent %u, veto broken %u (parents vetoed %u), corners outside the "
                    "range %u; today's rule: child above parent %u of %u\n",
                    t.Label, kPairs, above, vetoBroken, vetoed, unbounded, todayAbove, todayPairs);
        EXPECT_GT(todayAbove, todayPairs / 20u) << "positive control: the sampled pairs must expose today's rule";
        EXPECT_EQ(above, 0u) << "a child demands more than its parent";
        EXPECT_EQ(vetoBroken, 0u) << "the flat veto holds a parent and not its child";
        EXPECT_EQ(unbounded, 0u) << "a facet's corners spread wider than the range its demand reads";
    }
}
