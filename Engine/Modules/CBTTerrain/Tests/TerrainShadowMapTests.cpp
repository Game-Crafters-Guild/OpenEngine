// The terrain's sun-space clearance map (terrain_shadow.glsl, terrain_shadow_bake.comp,
// TerrainShadowGrid): the shared shader block runs here on the host, extracted from the shader the
// GPU compiles, so these tests execute the bake's per-sample steps and the receivers' lookup rather
// than a copy of them.
//
// - The bake emulates terrain_shadow_bake.comp's structure exactly (256 runs of 16 samples, the
//   run maxima, the Hillis-Steele suffix scan over the runs, the downwind walk) around the block's
//   functions, and stores floats where the GPU stores RG16F: half rounding moves a clearance by about
//   0.1 % of its value, which matters only where the clearance is near zero, at a shadow's edge,
//   which the comparisons exclude.
// - The reference is a brute-force march along the ray toward the sun over the bilinear height
//   field (the per-pixel march of the prototype, PR #2770, without its acceleration pyramid).

#include <gtest/gtest.h>

#include "CBTTerrain/TerrainShadowGrid.h"
#include "GlslShim.h"
#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace
{
using GameEngine::CBTTerrain::ComputeTerrainShadowGrid;
using GameEngine::CBTTerrain::kTerrainShadowLineCapacity;
using GameEngine::CBTTerrain::kTerrainShadowMaxSide;
using GameEngine::CBTTerrain::TerrainShadowDirtyLines;
using GameEngine::CBTTerrain::TerrainShadowExtent;
using GameEngine::CBTTerrain::TerrainShadowGrid;
using GameEngine::CBTTerrain::TerrainShadowLines;
using GameEngine::CBTTerrain::TerrainShadowMapSide;
using GameEngine::CBTTerrain::TerrainShadowTexel;

// A lattice height field: (Cells + 1)^2 samples over Size x Size metres, heights in metres.
struct HeightField
{
    uint32_t Cells = 0;
    float Size = 0.0f;
    std::vector<float> Heights;

    float At(uint32_t i, uint32_t j) const { return Heights[static_cast<size_t>(j) * (Cells + 1) + i]; }

    // The bilinear surface at (x, z) clamped to the terrain's edge: CBT_SampleHeight's lattice tap.
    float Sample(float x, float z) const
    {
        const float lx = std::clamp(x / Size, 0.0f, 1.0f) * static_cast<float>(Cells);
        const float lz = std::clamp(z / Size, 0.0f, 1.0f) * static_cast<float>(Cells);
        const uint32_t i = std::min(static_cast<uint32_t>(lx), Cells - 1u);
        const uint32_t j = std::min(static_cast<uint32_t>(lz), Cells - 1u);
        const float fx = lx - static_cast<float>(i);
        const float fz = lz - static_cast<float>(j);
        const float a = At(i, j) + (At(i + 1, j) - At(i, j)) * fx;
        const float b = At(i, j + 1) + (At(i + 1, j + 1) - At(i, j + 1)) * fx;
        return a + (b - a) * fz;
    }
};

HeightField MakeField(uint32_t cells, float size, const std::function<float(float, float)>& height)
{
    HeightField f;
    f.Cells = cells;
    f.Size = size;
    f.Heights.resize(static_cast<size_t>(cells + 1) * (cells + 1));
    const float cell = size / static_cast<float>(cells);
    for (uint32_t j = 0; j <= cells; ++j)
        for (uint32_t i = 0; i <= cells; ++i)
            f.Heights[static_cast<size_t>(j) * (cells + 1) + i] = height(i * cell, j * cell);
    return f;
}

// The map as the GPU holds it: Side x Side texels, line v in row v; the clearance and the occluder
// distance (the RG channels).
struct ClearanceMap
{
    uint32_t Side = 0;
    std::vector<float> Texels;
    std::vector<float> Distances;

    // Bilinear at a grid position in samples (texel centres at integers), clamped to the map:
    // textureLod with a linear clamp sampler at (gridCoord + 0.5) / side.
    static float Bilinear(const std::vector<float>& channel, uint32_t side, float u, float v)
    {
        const float cu = std::clamp(u, 0.0f, static_cast<float>(side - 1u));
        const float cv = std::clamp(v, 0.0f, static_cast<float>(side - 1u));
        const uint32_t i = std::min(static_cast<uint32_t>(cu), side - 2u);
        const uint32_t j = std::min(static_cast<uint32_t>(cv), side - 2u);
        const float fu = cu - static_cast<float>(i);
        const float fv = cv - static_cast<float>(j);
        auto at = [&channel, side](uint32_t x, uint32_t y) { return channel[static_cast<size_t>(y) * side + x]; };
        const float a = at(i, j) + (at(i + 1, j) - at(i, j)) * fu;
        const float b = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fu;
        return a + (b - a) * fv;
    }
};

// The two reads the shader block takes from its includer, bound to the test's current field and map.
const HeightField* gField = nullptr;
const ClearanceMap* gMap = nullptr;

namespace Shader
{
using GameEngine::GlslShim::acos;
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::dot;
using GameEngine::GlslShim::length;
using GameEngine::GlslShim::log2;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::mix;
using GameEngine::GlslShim::pow;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::vec2;
using GameEngine::GlslShim::vec4;

vec2 GE_TerrainShadowReadClearance(vec2 gridCoord)
{
    return {ClearanceMap::Bilinear(gMap->Texels, gMap->Side, gridCoord.x, gridCoord.y),
            ClearanceMap::Bilinear(gMap->Distances, gMap->Side, gridCoord.x, gridCoord.y)};
}
float GE_TerrainShadowReadGround(float x, float z)
{
    return gField->Sample(x, z);
}
#include "TerrainShadowExtracted.h"
} // namespace Shader

// The shader's grid for `grid` over the field, in the terrain's local frame (corner at the origin).
Shader::GE_TerrainShadowGrid ShaderGrid(const TerrainShadowGrid& grid, const HeightField& field)
{
    Shader::GE_TerrainShadowGrid g{};
    g.CenterX = 0.5f * field.Size;
    g.CenterZ = 0.5f * field.Size;
    g.TerrainX = 0.0f;
    g.TerrainZ = 0.0f;
    g.TerrainSizeX = field.Size;
    g.TerrainSizeZ = field.Size;
    g.SunX = grid.SunX;
    g.SunZ = grid.SunZ;
    g.TanElevation = grid.TanElevation;
    g.Texel = grid.Texel;
    g.UMin = grid.UMin;
    g.VMin = grid.VMin;
    g.SamplesU = static_cast<float>(grid.SamplesU);
    g.SamplesV = static_cast<float>(grid.SamplesV);
    return g;
}

constexpr uint32_t kRunLength = 16u;
constexpr uint32_t kRuns = 256u;
static_assert(kRunLength * kRuns == kTerrainShadowLineCapacity, "the kernel's line capacity");

// terrain_shadow_bake.comp for `lines` of the grid, writing into `map` (allocated by the caller);
// `tanLower` is the grid's TanLowerEdge.
void Bake(const Shader::GE_TerrainShadowGrid& g, float tanLower, TerrainShadowLines lines, ClearanceMap& map)
{
    const Shader::vec4 noCaster = Shader::GE_TerrainShadowCasterCarry(Shader::kGE_TerrainShadowNoCaster);
    for (uint32_t line = lines.First; line < lines.First + lines.Count; ++line)
    {
        const float lineF = static_cast<float>(line);
        std::array<Shader::vec4, kRuns> runTop{};
        for (uint32_t run = 0; run < kRuns; ++run)
        {
            Shader::vec4 top = noCaster;
            for (uint32_t i = 0; i < kRunLength; ++i)
            {
                const float caster =
                    Shader::GE_TerrainShadowCasterAndGround(g, static_cast<float>(run * kRunLength + i), lineF).x;
                top = Shader::GE_TerrainShadowHigherCarry(
                    top, Shader::GE_TerrainShadowCarryDownwind(g, Shader::GE_TerrainShadowCasterCarry(caster),
                                                               static_cast<float>(i), tanLower));
            }
            runTop[run] = top;
        }
        for (uint32_t offset = 1u; offset < kRuns; offset <<= 1u)
        {
            std::array<Shader::vec4, kRuns> merged = runTop;
            for (uint32_t run = 0; run + offset < kRuns; ++run)
                merged[run] = Shader::GE_TerrainShadowHigherCarry(
                    runTop[run], Shader::GE_TerrainShadowCarryDownwind(
                                     g, runTop[run + offset], static_cast<float>(offset * kRunLength), tanLower));
            runTop = merged;
        }
        for (uint32_t run = 0; run < kRuns; ++run)
        {
            Shader::vec4 carry = run + 1u < kRuns ? runTop[run + 1u] : noCaster;
            for (int i = static_cast<int>(kRunLength) - 1; i >= 0; --i)
            {
                const uint32_t k = run * kRunLength + static_cast<uint32_t>(i);
                const Shader::vec2 cg = Shader::GE_TerrainShadowCasterAndGround(g, static_cast<float>(k), lineF);
                const Shader::vec4 seen = Shader::GE_TerrainShadowCarryDownwind(g, carry, 1.0f, tanLower);
                if (static_cast<float>(k) <= g.SamplesU - 1.0f)
                {
                    const Shader::vec2 value = Shader::GE_TerrainShadowSampleValue(g, seen, cg.y, tanLower);
                    map.Texels[static_cast<size_t>(line) * map.Side + k] = value.x;
                    map.Distances[static_cast<size_t>(line) * map.Side + k] = value.y;
                }
                carry = Shader::GE_TerrainShadowHigherCarry(Shader::GE_TerrainShadowCasterCarry(cg.x), seen);
            }
        }
    }
}

struct Scene
{
    HeightField Field;
    TerrainShadowExtent Extent;
    TerrainShadowGrid Grid;
    Shader::GE_TerrainShadowGrid ShaderG;
    ClearanceMap Map;
};

TerrainShadowExtent ExtentOf(const HeightField& field)
{
    return {field.Size, field.Size, field.Cells, field.Cells};
}

// Toward the sun at `azimuthDeg` (0 = +X, 90 = +Z) and `elevationDeg` above the horizon.
std::array<float, 3> TowardSun(float azimuthDeg, float elevationDeg)
{
    const float a = azimuthDeg * GameEngine::Mathematics::Pi / 180.0f;
    const float e = elevationDeg * GameEngine::Mathematics::Pi / 180.0f;
    return {std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)};
}

// The scene's map for a sun at `azimuthDeg`, `elevationDeg` whose disc has the angular radius
// atan(tanHalfAngle) (0: a point sun).
Scene BakeScene(HeightField field, float azimuthDeg, float elevationDeg, float tanHalfAngle = 0.0f)
{
    Scene s;
    s.Field = std::move(field);
    s.Extent = ExtentOf(s.Field);
    const std::array<float, 3> sun = TowardSun(azimuthDeg, elevationDeg);
    s.Grid = *ComputeTerrainShadowGrid(s.Extent, sun.data(), tanHalfAngle);
    s.ShaderG = ShaderGrid(s.Grid, s.Field);
    s.Map.Side = TerrainShadowMapSide(s.Extent);
    s.Map.Texels.assign(static_cast<size_t>(s.Map.Side) * s.Map.Side, 0.0f);
    s.Map.Distances.assign(s.Map.Texels.size(), 0.0f);
    gField = &s.Field;
    Bake(s.ShaderG, s.Grid.TanLowerEdge, {0u, s.Grid.SamplesV}, s.Map);
    return s;
}

// The map's verdict for a receiver: the share of the sun that reaches it, for a sun of angular
// radius atan(tanHalfAngle) (0: a point sun, 1 lit or 0 shadow) and a penumbra radius bounded by
// `maxPenumbraRadius` metres.
float MapLit(const Scene& s, float x, float y, float z, bool onGround, float tanHalfAngle = 0.0f,
             float maxPenumbraRadius = Shader::kGE_TerrainShadowUncappedPenumbra)
{
    gField = &s.Field;
    gMap = &s.Map;
    return Shader::GE_TerrainShadowLit(s.ShaderG, x, y, z, onGround ? 1.0f : 0.0f, tanHalfAngle, maxPenumbraRadius);
}

// The march's steps per cell: a sixteenth of a cell resolves the map's samples; the view of a crest seen
// at a grazing angle needs a 256th, or a point a step below a sharp crest steps over it toward the sun.
constexpr float kMarchStepsPerCell = 16.0f;
constexpr float kCrestMarchStepsPerCell = 256.0f;

// The reference: march from the receiver toward the sun over the bilinear field in steps of
// 1 / stepsPerCell of a cell until the ray leaves the terrain's rectangle; occluded where the field rises
// above the ray. A receiver on the ground starts on the field; others keep their height, a base
// sunk into the terrain lifted onto it.
float MarchLit(const HeightField& f, float azimuthDeg, float elevationDeg, float x, float y, float z, bool onGround,
               float stepsPerCell)
{
    const std::array<float, 3> sun = TowardSun(azimuthDeg, elevationDeg);
    const float horizontal = std::sqrt(sun[0] * sun[0] + sun[2] * sun[2]);
    const float dx = sun[0] / horizontal;
    const float dz = sun[2] / horizontal;
    const float tanE = sun[1] / horizontal;
    const bool inside = x >= 0.0f && x <= f.Size && z >= 0.0f && z <= f.Size;
    float start = y;
    if (onGround)
        start = f.Sample(x, z);
    else if (inside)
        start = std::max(y, f.Sample(x, z));
    const float highest = *std::max_element(f.Heights.begin(), f.Heights.end());
    const float step = f.Size / static_cast<float>(f.Cells) / stepsPerCell;
    for (float t = step; start + t * tanE <= highest; t += step)
    {
        const float px = x + dx * t;
        const float pz = z + dz * t;
        const bool movingAway = (px < 0.0f && dx <= 0.0f) || (px > f.Size && dx >= 0.0f) ||
                                (pz < 0.0f && dz <= 0.0f) || (pz > f.Size && dz >= 0.0f);
        if (movingAway)
            return 1.0f;
        // Outside the rectangle nothing casts, but the ray may still enter it.
        const bool inRectangle = px >= 0.0f && px <= f.Size && pz >= 0.0f && pz <= f.Size;
        if (inRectangle && f.Sample(px, pz) > start + t * tanE)
            return 0.0f;
    }
    return 1.0f;
}

// The receivers lie on a square grid over the terrain and a margin around it (the sea), in two
// layers: on the ground (the terrain's own surface; at the base height off the terrain) and 3 m
// above the ground (a mesh; also at the base height off the terrain).
struct Comparison
{
    size_t Mismatches = 0;
    size_t Compared = 0;
    size_t Shadowed = 0;
};

// Map against march over every receiver, excluding those within `edgeCells` grid steps of a
// receiver of the same layer the march judges differently (the edge, where one texel of the map
// decides), and those whose neighbourhood leaves the receiver grid (where an edge just outside it
// cannot be seen). Returns the mismatches away from the edge, the receivers compared and how many of
// them the march shadows.
Comparison CompareWithMarch(const Scene& s, float azimuthDeg, float elevationDeg, float spacing, int edgeCells)
{
    const HeightField& f = s.Field;
    const float margin = 0.5f * f.Size;
    const float origin = -margin;
    const size_t n = static_cast<size_t>((f.Size + 2.0f * margin) / spacing) + 1u;
    auto coord = [origin, spacing](size_t i) { return origin + static_cast<float>(i) * spacing; };
    Comparison c;
    for (int layer = 0; layer < 2; ++layer)
    {
        std::vector<float> march(n * n);
        std::vector<float> height(n * n);
        std::vector<uint8_t> onGround(n * n);
        for (size_t j = 0; j < n; ++j)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const float x = coord(i);
                const float z = coord(j);
                const bool inside = x >= 0.0f && x <= f.Size && z >= 0.0f && z <= f.Size;
                const size_t at = j * n + i;
                onGround[at] = (inside && layer == 0) ? 1u : 0u;
                height[at] = inside ? f.Sample(x, z) + (layer == 0 ? 0.0f : 3.0f) : 0.0f;
                march[at] =
                    MarchLit(f, azimuthDeg, elevationDeg, x, height[at], z, onGround[at] != 0u, kMarchStepsPerCell);
            }
        }
        const size_t inner = static_cast<size_t>(edgeCells);
        for (size_t j = inner; j + inner < n; ++j)
        {
            for (size_t i = inner; i + inner < n; ++i)
            {
                const size_t at = j * n + i;
                bool nearEdge = false;
                for (int dj = -edgeCells; dj <= edgeCells && !nearEdge; ++dj)
                {
                    for (int di = -edgeCells; di <= edgeCells && !nearEdge; ++di)
                    {
                        const long long oi = static_cast<long long>(i) + di;
                        const long long oj = static_cast<long long>(j) + dj;
                        nearEdge = march[static_cast<size_t>(oj) * n + static_cast<size_t>(oi)] != march[at];
                    }
                }
                if (nearEdge)
                    continue;
                ++c.Compared;
                c.Shadowed += march[at] < 0.5f ? 1u : 0u;
                if (MapLit(s, coord(i), height[at], coord(j), onGround[at] != 0u) != march[at])
                    ++c.Mismatches;
            }
        }
    }
    return c;
}

// 64 m terrains at 1 m cells: a terrace (a 10 m step across x = 32), a ridge along z, a cone.
HeightField Terrace()
{
    return MakeField(64u, 64.0f, [](float x, float) { return x < 32.0f ? 10.0f : 0.0f; });
}
HeightField Ridge()
{
    return MakeField(64u, 64.0f, [](float x, float) { return 12.0f * std::exp(-0.5f * (x - 32.0f) * (x - 32.0f) / 16.0f); });
}
HeightField Cone()
{
    return MakeField(64u, 64.0f, [](float x, float z) {
        const float r = std::sqrt((x - 32.0f) * (x - 32.0f) + (z - 32.0f) * (z - 32.0f));
        return std::max(0.0f, 20.0f * (1.0f - r / 15.0f));
    });
}
} // namespace

// The map and the march agree on every receiver away from a shadow's edge: on the ground, above it
// and around the terrain, for three shapes at five azimuths and three elevations.
TEST(TerrainShadowMap, MatchesTheMarchAwayFromTheEdge)
{
    const std::array<float, 5> azimuths = {0.0f, 30.0f, 45.0f, 90.0f, 180.0f};
    const std::array<float, 3> elevations = {5.0f, 15.0f, 45.0f};
    const std::array<std::pair<const char*, HeightField (*)()>, 3> shapes = {
        std::pair{"terrace", &Terrace}, std::pair{"ridge", &Ridge}, std::pair{"cone", &Cone}};
    for (const auto& [name, make] : shapes)
    {
        for (float azimuth : azimuths)
        {
            for (float elevation : elevations)
            {
                const Scene s = BakeScene(make(), azimuth, elevation);
                // Receivers every 0.75 m; the edge band is two of them, one and a half texels.
                const Comparison c = CompareWithMarch(s, azimuth, elevation, 0.75f, 2);
                EXPECT_EQ(c.Mismatches, 0u) << name << " azimuth " << azimuth << " elevation " << elevation << ": "
                                            << c.Mismatches << " of " << c.Compared << " receivers differ";
                EXPECT_GT(c.Shadowed, 0u) << name << " azimuth " << azimuth << " elevation " << elevation
                                          << ": no receiver in shadow, so the comparison shows nothing";
            }
        }
    }
}

// The bake's scan (the runs, their suffix combination, the walk) stores, at every sample of every
// line, the brute-force maximum over the casters upwind of it: top(k) = max over j > k of
// (h(j) - (j - k) * texel * tan(elevation)), less the ground at k; and the distance to the j that
// attains it, (j - k) * texel, wherever one caster attains it clearly.
TEST(TerrainShadowMap, ScanEqualsTheBruteForceMaximum)
{
    const Scene s = BakeScene(Cone(), 30.0f, 15.0f);
    gField = &s.Field;
    const float step = s.ShaderG.Texel * s.ShaderG.TanElevation;
    size_t checked = 0;
    size_t distancesChecked = 0;
    for (uint32_t line = 0; line < s.Grid.SamplesV; ++line)
    {
        std::vector<float> caster(s.Grid.SamplesU);
        std::vector<float> ground(s.Grid.SamplesU);
        for (uint32_t k = 0; k < s.Grid.SamplesU; ++k)
        {
            const Shader::vec2 cg = Shader::GE_TerrainShadowCasterAndGround(s.ShaderG, static_cast<float>(k),
                                                                            static_cast<float>(line));
            caster[k] = cg.x;
            ground[k] = cg.y;
        }
        for (uint32_t k = 0; k < s.Grid.SamplesU; ++k)
        {
            float top = Shader::kGE_TerrainShadowNoCaster;
            float runnerUp = Shader::kGE_TerrainShadowNoCaster;
            uint32_t argmax = 0;
            for (uint32_t j = k + 1; j < s.Grid.SamplesU; ++j)
            {
                const float candidate = caster[j] - static_cast<float>(j - k) * step;
                if (candidate > top)
                {
                    runnerUp = top;
                    top = candidate;
                    argmax = j;
                }
                else
                {
                    runnerUp = std::max(runnerUp, candidate);
                }
            }
            const float expected = std::clamp(top - ground[k], -Shader::kGE_TerrainShadowClearanceLimit,
                                              Shader::kGE_TerrainShadowClearanceLimit);
            const size_t at = static_cast<size_t>(line) * s.Map.Side + k;
            ASSERT_NEAR(s.Map.Texels[at], expected, 1e-3f) << "line " << line << " sample " << k;
            ++checked;
            if (top > Shader::kGE_TerrainShadowNoCaster && top - runnerUp > 1e-2f)
            {
                ASSERT_NEAR(s.Map.Distances[at], static_cast<float>(argmax - k) * s.ShaderG.Texel, 1e-3f)
                    << "line " << line << " sample " << k;
                ++distancesChecked;
            }
        }
    }
    EXPECT_GT(checked, 1000u);
    EXPECT_GT(distancesChecked, 1000u);
}

// An edit re-bakes only the lines whose samples read the edited heights, and that leaves the map
// exactly as a full bake of the edited terrain would.
TEST(TerrainShadowMap, DirtyLineRebakeEqualsAFullRebake)
{
    Scene s = BakeScene(Ridge(), 30.0f, 15.0f);
    HeightField edited = s.Field;
    const float minU = 0.40f, minV = 0.60f, maxU = 0.50f, maxV = 0.70f;
    for (uint32_t j = 0; j <= edited.Cells; ++j)
    {
        for (uint32_t i = 0; i <= edited.Cells; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(edited.Cells);
            const float v = static_cast<float>(j) / static_cast<float>(edited.Cells);
            if (u >= minU && u <= maxU && v >= minV && v <= maxV)
                edited.Heights[static_cast<size_t>(j) * (edited.Cells + 1) + i] += 8.0f;
        }
    }
    const TerrainShadowLines lines = TerrainShadowDirtyLines(s.Grid, s.Extent, minU, minV, maxU, maxV);
    ASSERT_GT(lines.Count, 0u);
    EXPECT_LT(lines.Count, s.Grid.SamplesV / 2u) << "an edit re-bakes a band of lines, not the map";

    s.Field = edited;
    gField = &s.Field;
    Bake(s.ShaderG, s.Grid.TanLowerEdge, lines, s.Map);

    const Scene full = BakeScene(edited, 30.0f, 15.0f);
    size_t differing = 0;
    for (size_t i = 0; i < s.Map.Texels.size(); ++i)
        differing += (s.Map.Texels[i] != full.Map.Texels[i] || s.Map.Distances[i] != full.Map.Distances[i]) ? 1u : 0u;
    EXPECT_EQ(differing, 0u) << "texels the partial re-bake left stale";
}

// The edit's lines are the ones crossing its rectangle across the sun: a rectangle at the far side
// of the terrain from another reaches other lines.
TEST(TerrainShadowMap, DirtyLinesFollowTheRectangle)
{
    const TerrainShadowExtent extent{64.0f, 64.0f, 64u, 64u};
    const std::array<float, 3> sun = TowardSun(0.0f, 20.0f); // u along +X, lines across z
    const TerrainShadowGrid grid = *ComputeTerrainShadowGrid(extent, sun.data(), 0.0f);
    const TerrainShadowLines low = TerrainShadowDirtyLines(grid, extent, 0.1f, 0.10f, 0.9f, 0.15f);
    const TerrainShadowLines high = TerrainShadowDirtyLines(grid, extent, 0.1f, 0.85f, 0.9f, 0.90f);
    ASSERT_GT(low.Count, 0u);
    ASSERT_GT(high.Count, 0u);
    EXPECT_LT(low.First + low.Count, high.First) << "z 6-10 m and z 54-58 m share no line";
    EXPECT_EQ(TerrainShadowDirtyLines(grid, extent, 0.5f, 0.5f, 0.5f, 0.6f).Count, 0u) << "an empty rectangle";
}

// The map's size: one lattice cell per texel while the terrain's diagonal fits the cap, a coarser
// texel beyond it, and every sun's grid within the map.
TEST(TerrainShadowMap, GridFitsTheMapAtEveryAzimuth)
{
    const TerrainShadowExtent lakeside{576.0f, 576.0f, 576u, 576u};
    EXPECT_EQ(TerrainShadowMapSide(lakeside), 816u);
    EXPECT_FLOAT_EQ(TerrainShadowTexel(lakeside), 1.0f);
    const TerrainShadowExtent r4{4096.0f, 4096.0f, 4096u, 4096u};
    EXPECT_EQ(TerrainShadowMapSide(r4), kTerrainShadowMaxSide);
    EXPECT_NEAR(TerrainShadowTexel(r4), 1.4146f, 1e-3f);
    for (const TerrainShadowExtent& extent : {lakeside, r4, TerrainShadowExtent{1000.0f, 250.0f, 500u, 125u}})
    {
        for (float azimuth = 0.0f; azimuth < 360.0f; azimuth += 7.5f)
        {
            const std::array<float, 3> sun = TowardSun(azimuth, 25.0f);
            const auto grid = ComputeTerrainShadowGrid(extent, sun.data(), 0.0f);
            ASSERT_TRUE(grid.has_value());
            EXPECT_LE(grid->SamplesU, TerrainShadowMapSide(extent)) << "azimuth " << azimuth;
            EXPECT_LE(grid->SamplesV, TerrainShadowMapSide(extent)) << "azimuth " << azimuth;
            EXPECT_LE(grid->SamplesU, kTerrainShadowLineCapacity);
        }
    }
}

// No map below the horizon or near the zenith: the terrain then casts no shadow at all.
TEST(TerrainShadowMap, NoGridWithoutALowEnoughSun)
{
    const TerrainShadowExtent extent{64.0f, 64.0f, 64u, 64u};
    const std::array<float, 3> below = TowardSun(10.0f, -3.0f);
    const std::array<float, 3> zenith = TowardSun(10.0f, 89.8f);
    const std::array<float, 3> high = TowardSun(10.0f, 89.0f);
    EXPECT_FALSE(ComputeTerrainShadowGrid(extent, below.data(), 0.0f).has_value());
    EXPECT_FALSE(ComputeTerrainShadowGrid(extent, zenith.data(), 0.0f).has_value());
    EXPECT_TRUE(ComputeTerrainShadowGrid(extent, high.data(), 0.0f).has_value());
}

namespace
{
// A 128 m terrain at 1 m cells with a 10 m plateau over x < 32: a straight edge whose shadow falls
// across level ground toward +X under a sun on the -X side.
HeightField Plateau()
{
    return MakeField(128u, 128.0f, [](float x, float) { return x < 32.0f ? 10.0f : 0.0f; });
}

// The penumbra on the ground across the plateau's shadow edge, along z = 64: the last receiver in
// full shadow and the first fully lit one, walked in steps of 5 mm.
struct Penumbra
{
    float LastShadowed = 0.0f;
    float FirstLit = 0.0f;
};

Penumbra MeasurePenumbra(const Scene& s, float tanHalfAngle, float maxPenumbraRadius)
{
    Penumbra p{};
    for (float x = 50.0f; x < 90.0f; x += 0.005f)
    {
        const float lit = MapLit(s, x, s.Field.Sample(x, 64.0f), 64.0f, true, tanHalfAngle, maxPenumbraRadius);
        if (lit <= 0.0f)
            p.LastShadowed = x;
        if (lit >= 1.0f && p.FirstLit == 0.0f)
            p.FirstLit = x;
    }
    return p;
}
} // namespace

// The terrain's shadow edge is the sun's disc cut by the edge ray over the plateau, as the mesh
// shadows model it: on level ground the penumbra is 2 * r / sin(elevation) wide, with
// r = (D / cos(elevation)) * tan(angular radius) the disc's radius across the ray at the occluder's
// distance D; a bound on r caps it as the cascades' Max Penumbra caps a mesh's; a point sun leaves
// the edge hard.
TEST(TerrainShadowMap, PenumbraWidthMatchesTheSunsDiscAtTheOccludersDistance)
{
    constexpr float kElevationDeg = 15.0f;
    const float tanHalfAngle = std::tan(2.0f * GameEngine::Mathematics::Pi / 180.0f); // a 4 degree sun
    const Scene s = BakeScene(Plateau(), 180.0f, kElevationDeg, tanHalfAngle);
    const float elevation = kElevationDeg * GameEngine::Mathematics::Pi / 180.0f;
    // The crest is the plateau's last lattice sample (x = 31); the grid's samples place it within one
    // texel, which moves the occluder's distance, and so the uncapped width, by up to 0.3 m.
    const float crestX = 31.0f;
    const float distance = 10.0f / std::tan(elevation);
    const float edgeX = crestX + distance;

    const Penumbra soft = MeasurePenumbra(s, tanHalfAngle, Shader::kGE_TerrainShadowUncappedPenumbra);
    const float radius = distance / std::cos(elevation) * tanHalfAngle;
    EXPECT_NEAR(soft.FirstLit - soft.LastShadowed, 2.0f * radius / std::sin(elevation), 0.3f)
        << "shadowed to x " << soft.LastShadowed << ", lit from x " << soft.FirstLit;
    EXPECT_NEAR(0.5f * (soft.FirstLit + soft.LastShadowed), edgeX, s.ShaderG.Texel) << "the penumbra centres on the edge";

    constexpr float kMaxPenumbraRadius = 0.2f;
    const Penumbra capped = MeasurePenumbra(s, tanHalfAngle, kMaxPenumbraRadius);
    EXPECT_NEAR(capped.FirstLit - capped.LastShadowed, 2.0f * kMaxPenumbraRadius / std::sin(elevation), 0.02f)
        << "shadowed to x " << capped.LastShadowed << ", lit from x " << capped.FirstLit;

    const Penumbra hard = MeasurePenumbra(s, 0.0f, Shader::kGE_TerrainShadowUncappedPenumbra);
    EXPECT_LE(hard.FirstLit - hard.LastShadowed, 0.0051f) << "a point sun's edge is one step";
}

namespace
{
// Ridges along z, a triangle wave across x: 2 m period, from 0 to 1 m, 45-degree faces (the R3 evidence
// field), on a 64 m terrain at 0.25 m cells.
HeightField RidgeRows()
{
    return MakeField(256u, 64.0f, [](float x, float) {
        const float phase = x / 2.0f - std::floor(x / 2.0f);
        return phase < 0.5f ? 2.0f * phase : 2.0f - 2.0f * phase;
    });
}

// What a view sees of the sun on the ground of a patch, over the points of the patch's mean plane at
// y = meanY the view's rays aim at: the lit share of all the patch's ground (All) and of the
// ground the rays reach first (Seen), both ray-marched (the truth); and the lit field's two channels
// over the same ground from the baked map (FieldLit, FieldOctaves: what terrain_shadow_field_lit.comp
// averages).
struct PatchShares
{
    float All = 0.0f;
    float Seen = 0.0f;
    float FieldLit = 0.0f;
    float FieldOctaves = 0.0f;
};

constexpr float kPatchX0 = 31.0f;
constexpr float kPatchX1 = 35.0f;
constexpr float kPatchZ = 32.0f;

PatchShares SharesFromEye(const Scene& s, float sunAzimuthDeg, float sunElevationDeg, float meanY, float eyeX,
                          float eyeY)
{
    const HeightField& f = s.Field;
    const float step = f.Size / static_cast<float>(f.Cells) / kCrestMarchStepsPerCell;
    PatchShares shares{};
    int count = 0;
    for (float z = kPatchZ - 0.5f; z <= kPatchZ + 0.5f; z += 0.5f)
    {
        for (float x = kPatchX0; x < kPatchX1; x += 0.05f)
        {
            const float ground = f.Sample(x, z);
            shares.All += MarchLit(f, sunAzimuthDeg, sunElevationDeg, x, ground, z, true, kCrestMarchStepsPerCell);
            gField = &s.Field;
            gMap = &s.Map;
            const Shader::vec2 margin = Shader::GE_TerrainShadowMargin(s.ShaderG, x, ground, z, 1.0f);
            if (margin.x > 0.0f)
                shares.FieldOctaves += std::log2(std::max(margin.y, s.ShaderG.Texel) / s.ShaderG.Texel);
            else
                shares.FieldLit += 1.0f;
            const float dx = x - eyeX;
            const float dy = meanY - eyeY;
            const float dz = z - kPatchZ;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            float hitX = x;
            float hitZ = z;
            for (float t = step; t < length + 4.0f; t += step)
            {
                const float px = eyeX + dx / length * t;
                const float py = eyeY + dy / length * t;
                const float pz = kPatchZ + dz / length * t;
                if (f.Sample(px, pz) >= py)
                {
                    hitX = px;
                    hitZ = pz;
                    break;
                }
            }
            shares.Seen += MarchLit(f, sunAzimuthDeg, sunElevationDeg, hitX, f.Sample(hitX, hitZ), hitZ, true,
                                    kCrestMarchStepsPerCell);
            ++count;
        }
    }
    const float sampleCount = static_cast<float>(count);
    shares.All /= sampleCount;
    shares.Seen /= sampleCount;
    shares.FieldLit /= sampleCount;
    shares.FieldOctaves /= sampleCount;
    return shares;
}

constexpr float kGrazingSunElevationDeg = 20.0f;

// A sun-lit view of a patch: the eye, the sun, and what the lit field shows there.
struct GrazingCase
{
    const char* Name = "";
    float SunAzimuthDeg = 0.0f;
    float EyeX = 0.0f;
    float MeanY = 0.0f;     // the patch's mean plane
    float EyeHeight = 0.0f; // above it
    float SunElevationDeg = kGrazingSunElevationDeg;
};

// A draw level where the draw shows none of the patch's relief, so the shown share is the whole model.
constexpr float kReliefUndrawn = 2.0f;

// The share the lit field shows a view of the patch (GE_TerrainShadowFieldShown, the arithmetic both
// shadow modes run) at the draw level `drawLevel`, against the truth. The eye looks along +X at the
// patch's middle; its footprint is the patch's length.
struct GrazingRead
{
    PatchShares Shares;
    float ViewCos = 0.0f;
    float Local = 0.0f;
    float Shown = 0.0f;
};

GrazingRead ReadGrazing(const Scene& s, const GrazingCase& c, float drawLevel = kReliefUndrawn)
{
    GrazingRead r{};
    r.Shares = SharesFromEye(s, c.SunAzimuthDeg, c.SunElevationDeg, c.MeanY, c.EyeX, c.MeanY + c.EyeHeight);
    const float dx = 0.5f * (kPatchX0 + kPatchX1) - c.EyeX;
    r.ViewCos = c.EyeHeight / std::sqrt(dx * dx + c.EyeHeight * c.EyeHeight);
    const float footprintSamples = (kPatchX1 - kPatchX0) / s.ShaderG.Texel;
    const Shader::vec2 read(r.Shares.FieldLit, r.Shares.FieldOctaves);
    r.Local = Shader::GE_TerrainShadowLocalShare(read.x, read.y, footprintSamples);
    r.Shown = Shader::GE_TerrainShadowFieldShown(s.ShaderG, read, footprintSamples, Shader::vec2(-1.0f, 0.0f),
                                                 r.ViewCos, drawLevel);
    std::printf("%s: view cos %.4f, draw level %.2f, lit share of all the ground %.3f (field %.3f, local shadow "
                "%.3f), of what the view sees %.3f, shown %.3f\n",
                c.Name, r.ViewCos, drawLevel, r.Shares.All, r.Shares.FieldLit, r.Local, r.Shares.Seen, r.Shown);
    return r;
}

// A 64 m plain at 0.25 m cells with a cliff along z > 50 whose top rises along x: under a sun from +Z
// its shadow's edge crosses the patch along x, so about half the patch lies in a shadow cast from
// 18 m away, on flat ground.
HeightField PlainUnderACliff()
{
    return MakeField(256u, 64.0f, [](float x, float z) {
        constexpr float kEdgeHeight = 6.55f; // at x = 33 the shadow ends at z = 32 under a 20 degree sun
        return z > 50.0f ? kEdgeHeight + 0.27f * (x - 33.0f) : 0.0f;
    });
}

// A 64 m plain at 0.25 m cells with a wall over x in [x0, x1] whose top casts its shadow's edge at
// x = 33 under a sun along X at kGrazingSunElevationDeg: about half the patch lies in a shadow cast
// from more than 25 m away, on flat ground.
HeightField PlainBesideAWall(float x0, float x1)
{
    constexpr float kEdgeX = 33.0f;
    const float tanSun = std::tan(kGrazingSunElevationDeg * GameEngine::Mathematics::Pi / 180.0f);
    const float height = (x1 < kEdgeX ? kEdgeX - x1 : x0 - kEdgeX) * tanSun;
    return MakeField(256u, 64.0f, [=](float x, float) {
        return x >= x0 && x <= x1 ? height : 0.0f;
    });
}

// A flat-ground cast shadow read from the eye: the view sees every point of the ground the field
// averages, so the share shown is the field's.
void ExpectCastShadowShownAsItIs(const GrazingRead& plain)
{
    ASSERT_GT(plain.Shares.All, 0.2f) << "the cast shadow's edge must cross the patch";
    ASSERT_LT(plain.Shares.All, 0.8f) << "the cast shadow's edge must cross the patch";
    EXPECT_NEAR(plain.Shares.Seen, plain.Shares.All, 1.0e-3f) << "flat ground hides none of itself";
    EXPECT_NEAR(plain.Shown, plain.Shares.FieldLit, 1.0e-3f);
    EXPECT_LE(std::fabs(plain.Shown - plain.Shares.Seen), 0.05f);
}
} // namespace

// The lit field averages the sun over all the ground a pixel covers. The ridge field (the R3 evidence
// poses at eye height) seen at a grazing angle: its crests hide the ground behind them, and which ground
// that is depends on the sun's side. With the sun behind the eye the view looks down the shadows and
// sees lit faces only; looking into the sun it sees the faces turned from it. The share shown follows
// the truth on both sides, within 0.1 at 3 degrees; seen steeply with the sun behind, where the view
// sees both faces, it moves from the field's share toward the truth, and past it by no more than the
// map's own error.
TEST(TerrainShadowMap, LitFieldShowsWhatAGrazingViewSeesOnEitherSideOfTheSun)
{
    const Scene behind = BakeScene(RidgeRows(), 180.0f, kGrazingSunElevationDeg); // sun toward -X
    const Scene ahead = BakeScene(RidgeRows(), 0.0f, kGrazingSunElevationDeg);    // sun toward +X
    const GrazingRead frontLit = ReadGrazing(behind, {"sun behind the eye, grazing", 180.0f, 2.0f, 0.5f, 1.7f});
    EXPECT_GT(frontLit.Shares.Seen - frontLit.Shares.All, 0.2f) << "the instrument must see the crests hide the troughs";
    EXPECT_LE(std::fabs(frontLit.Shown - frontLit.Shares.Seen), 0.1f);

    const GrazingRead backLit = ReadGrazing(ahead, {"sun ahead of the eye, grazing", 0.0f, 2.0f, 0.5f, 1.7f});
    EXPECT_GT(backLit.Shares.All - backLit.Shares.Seen, 0.05f) << "the instrument must see the lit faces hidden";
    EXPECT_LE(std::fabs(backLit.Shown - backLit.Shares.Seen), 0.1f);

    const GrazingRead steep = ReadGrazing(behind, {"sun behind the eye, steep", 180.0f, 13.0f, 0.5f, 34.64f});
    EXPECT_LT(std::fabs(steep.Shown - steep.Shares.Seen), std::fabs(steep.Shares.FieldLit - steep.Shares.Seen));
    EXPECT_GE(steep.Shown, steep.Shares.FieldLit);
    EXPECT_LE(steep.Shown - steep.Shares.Seen, steep.Shares.FieldLit - steep.Shares.All)
        << "past the truth by no more than the map's own lit share reads above the march";
}

// The view's masking that takes lit ground away is counted for the relief the draw no longer shows:
// none while the draw shows the level-0 relief or its first level's (to draw level 1), all of it from
// draw level 2, and in between in proportion.
TEST(TerrainShadowMap, ViewMaskingCountsOnlyTheReliefTheDrawNoLongerShows)
{
    for (const float drawn : {-6.0f, 0.0f, 0.75f, 1.0f})
        EXPECT_EQ(Shader::GE_TerrainShadowUndrawnRelief(drawn), 0.0f) << drawn;
    for (const float undrawn : {2.0f, 2.5f, 12.0f})
        EXPECT_EQ(Shader::GE_TerrainShadowUndrawnRelief(undrawn), 1.0f) << undrawn;
    EXPECT_FLOAT_EQ(Shader::GE_TerrainShadowUndrawnRelief(1.25f), 0.25f);
    EXPECT_FLOAT_EQ(Shader::GE_TerrainShadowUndrawnRelief(1.5f), 0.5f);
}

// The ground a pixel covers, one rule for both shadow modes: the head-on footprint widened by 1 / cos
// along the draw's level rule (cbt_height_chain.glsl CBT_GrazingShare): not at draw level 0 and
// nearer, fully from draw level 1, between draw levels 0 and 1 by 1 / cos raised to the draw level,
// and at most 64 times.
TEST(TerrainShadowMap, FootprintWidensByTheDrawsLevelRule)
{
    constexpr float kHeadOn = 0.5f;
    constexpr float kCos = 1.0f / 16.0f;
    for (const float drawn : {-6.0f, 0.0f})
        EXPECT_EQ(Shader::GE_TerrainShadowFootprint(kHeadOn, kCos, drawn), kHeadOn) << drawn;
    for (const float grazing : {1.0f, 4.0f})
        EXPECT_FLOAT_EQ(Shader::GE_TerrainShadowFootprint(kHeadOn, kCos, grazing), 8.0f) << grazing;
    EXPECT_FLOAT_EQ(Shader::GE_TerrainShadowFootprint(kHeadOn, kCos, 0.5f), 2.0f);
    EXPECT_FLOAT_EQ(Shader::GE_TerrainShadowFootprint(kHeadOn, 0.001f, 1.0f), 32.0f) << "at most 64 times";
    EXPECT_EQ(Shader::GE_TerrainShadowFootprint(kHeadOn, 1.0f, 3.0f), kHeadOn) << "a view straight down";
}

// The ridge field at the R3 evidence sun (15 degrees) seen at a grazing angle. With the sun behind the
// eye the view looks down the shadows: the field shows the lit faces the view sees at every draw
// level, so the horizon of the eye-height frame carries no dark line where the draw level is low.
// Looking into the sun the view's masking takes lit ground away, which is counted only for the relief
// the draw no longer shows: the share falls from the field's own at draw level 1 to the model's at 2.
// The numbers pin the arithmetic both shadow modes run.
TEST(TerrainShadowMap, FieldShownTakesLitGroundAwayOnlyForTheReliefTheDrawNoLongerShows)
{
    constexpr float kR3SunElevationDeg = 15.0f;
    const Scene behind = BakeScene(RidgeRows(), 180.0f, kR3SunElevationDeg); // sun toward -X
    const GrazingCase eye{"sun behind the eye at 15 degrees, grazing", 180.0f, 2.0f, 0.5f, 1.7f, kR3SunElevationDeg};
    for (const float drawLevel : {0.0f, 1.0f, 1.5f, 2.0f})
    {
        const GrazingRead read = ReadGrazing(behind, eye, drawLevel);
        EXPECT_NEAR(read.Shares.FieldLit, 0.309f, 1.0e-3f);
        EXPECT_NEAR(read.Shares.Seen, 1.0f, 1.0e-3f);
        EXPECT_NEAR(read.Shown, 1.0f, 1.0e-3f) << "draw level " << drawLevel;
    }

    const Scene ahead = BakeScene(RidgeRows(), 0.0f, kR3SunElevationDeg); // sun toward +X
    const GrazingCase into{"sun ahead of the eye at 15 degrees, grazing", 0.0f, 2.0f, 0.5f, 1.7f, kR3SunElevationDeg};
    const GrazingRead drawn = ReadGrazing(ahead, into, 1.0f);
    const GrazingRead half = ReadGrazing(ahead, into, 1.5f);
    const GrazingRead undrawn = ReadGrazing(ahead, into, 2.0f);
    EXPECT_EQ(drawn.Shown, drawn.Shares.FieldLit) << "where the draw shows the relief the field is shown as it is";
    EXPECT_NEAR(half.Shown, 0.5f * (drawn.Shown + undrawn.Shown), 1.0e-6f);
    EXPECT_LT(undrawn.Shown, drawn.Shown);
    EXPECT_LE(std::fabs(undrawn.Shown - undrawn.Shares.Seen), 0.1f);
}

// The ridge field with the sun 45 degrees around from behind the eye, seen at a grazing angle: the
// ridges cross the view and the sun alike, so the view still looks down the shadows and sees lit
// faces. The view's and the sun's masking favor the same ground while their azimuths are near, so the
// share shown follows the truth within 0.1.
TEST(TerrainShadowMap, LitFieldShowsWhatAGrazingViewSeesWithTheSunPartWayAround)
{
    constexpr float kSunAzimuthDeg = 135.0f; // toward -X and +Z: 45 degrees from behind the eye
    const Scene aside = BakeScene(RidgeRows(), kSunAzimuthDeg, kGrazingSunElevationDeg);
    const GrazingRead read = ReadGrazing(aside, {"sun 45 degrees around from behind the eye, grazing", kSunAzimuthDeg,
                                                 2.0f, 0.5f, 1.7f});
    EXPECT_GT(read.Shares.Seen - read.Shares.All, 0.2f) << "the instrument must see the crests hide the troughs";
    EXPECT_LE(std::fabs(read.Shown - read.Shares.Seen), 0.1f);
}

// The ground the view and the sun both see, in the height-direction correlated form
// 1 / (1 + max(Ls, Lv) + c min(Ls, Lv)), with c = 4.41 phi / (4.41 phi + 1) for phi the horizontal angle
// between the eye and the sun, at most a quarter turn. Half the ground in local shadow gives the sun's
// Ls = 1; a view at half the sun's tangent gives the view's Lv = 2 and a visible share of 1 / 3, so a
// lit share of 0.4 is shown as 2.4 / (3 + c): c = 0 with the sun behind the eye, 0.776 at 45
// degrees around, 0.874 from the side.
TEST(TerrainShadowMap, SeenShareCorrelatesTheTwoMaskingsByTheirAzimuths)
{
    constexpr float kLit = 0.4f;
    constexpr float kLocal = 0.5f;
    constexpr float kTanSun = 0.25f;
    constexpr float kTanView = 0.125f;
    EXPECT_NEAR(Shader::GE_TerrainShadowSeenShare(kLit, kLocal, kTanView, kTanSun, 1.0f), 0.8f, 1.0e-5f)
        << "the sun behind the eye: 2.4 / 3";
    EXPECT_NEAR(Shader::GE_TerrainShadowSeenShare(kLit, kLocal, kTanView, kTanSun, std::sqrt(0.5f)), 0.6356f, 1.0e-4f)
        << "the sun 45 degrees around: 2.4 / 3.776";
    EXPECT_NEAR(Shader::GE_TerrainShadowSeenShare(kLit, kLocal, kTanView, kTanSun, 0.0f), 0.6195f, 1.0e-4f)
        << "the sun to the side: 2.4 / 3.874";
}

// A shadow cast onto flat ground from farther than the pixel's footprint hides nothing from the view:
// the view sees every point of the ground the field averages, so the share shown is the field's, with
// the sun to the side, behind the eye or ahead of it. Behind and ahead, the model would move a local
// shadow's share all the way (to 1 with the sun behind, to 0 ahead): the field's occluder distances
// are what tell this shadow from a local one.
TEST(TerrainShadowMap, LitFieldShowsACastShadowOnFlatGroundAsItIs)
{
    const Scene side = BakeScene(PlainUnderACliff(), 90.0f, kGrazingSunElevationDeg); // sun toward +Z
    const GrazingCase sideEye{"flat ground in a cast shadow, sun to the side", 90.0f, 2.0f, 0.0f, 1.2f};
    ExpectCastShadowShownAsItIs(ReadGrazing(side, sideEye));

    // The wall stands behind the eye, which looks along +X from x = 4.
    const Scene behind = BakeScene(PlainBesideAWall(0.0f, 2.0f), 180.0f, kGrazingSunElevationDeg); // toward -X
    const GrazingCase behindEye{"flat ground in a cast shadow, sun behind the eye", 180.0f, 4.0f, 0.0f, 1.2f};
    ExpectCastShadowShownAsItIs(ReadGrazing(behind, behindEye));

    const Scene ahead = BakeScene(PlainBesideAWall(60.0f, 64.0f), 0.0f, kGrazingSunElevationDeg); // toward +X
    const GrazingCase aheadEye{"flat ground in a cast shadow, sun ahead of the eye", 0.0f, 4.0f, 0.0f, 1.2f};
    ExpectCastShadowShownAsItIs(ReadGrazing(ahead, aheadEye));
}
