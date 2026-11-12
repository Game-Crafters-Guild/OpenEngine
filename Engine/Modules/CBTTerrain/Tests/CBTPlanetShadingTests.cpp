// CPU oracles for planet shading quality (plan §planet-shading). Pure math — no Vulkan
// device — so they always run, and each discriminates a real failure mode of the sphere
// shading path (they FAIL against the pre-change behaviour):
//   * Analytic relief gradient == central finite difference (the gradient is correct — a
//     facet/flat normal has NO gradient and fails).
//   * Analytic shading normal == the geometric normal of the displaced surface (the whole
//     N = normalize(dir - gradTangential(h)/(R+h)) formula is correct, and it is NOT the
//     radial normal where relief has slope).
//   * The normal is a function of world DIRECTION only, so it is bit-for-bit identical from
//     either cube face at a shared edge/corner — seam-free (the C4/C7 cross-face law).
//   * The multi-octave relief keeps the +/- 1.5*amplitude envelope for any octave count
//     (so CBTSphereDomainTests' on-shell bound stays valid) while adding real variation.

#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "Mathematics/Angle.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine::CBTTerrain;

namespace
{
using GameEngine::ColorLinear;
using GameEngine::ColorUtils::LinearRec709Luminance;
using GameEngine::Mathematics::ToDegrees;
using GameEngine::Mathematics::Vector3;

// The displaced sphere surface point for a (not-necessarily-unit) direction — normalize,
// displace by the multi-octave relief along it. The geometric-normal oracle differences it.
Vector3 SurfacePoint(const Vector3& d, float radius, float amp, float freq, uint32_t oct)
{
    const Vector3 n = d.Normalize();
    const float h = PlanetRelief(n.x, n.y, n.z, amp, freq, oct);
    return n * (radius + h);
}

// Independent brute-force occlusion: does the segment C->P pass through the floor-sphere of
// radius r (centred at the origin) before reaching P? The entry intersection parameter t0 must
// lie strictly inside (0,1). This is the geometric ground truth the cone predicate must match.
bool BruteForceOccluded(const Vector3& P, const Vector3& C, float r)
{
    const Vector3 d = P - C;
    const float a = Vector3::Dot(d, d);
    if (a <= 0.0f)
        return false;
    const float b = 2.0f * Vector3::Dot(C, d);
    const float c = Vector3::Dot(C, C) - r * r;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
        return false;
    const float t0 = (-b - std::sqrt(disc)) / (2.0f * a);
    return t0 > 1e-4f && t0 < 1.0f - 1e-4f;
}

// Signed distance to the cone-test occlusion boundary (< 0 => the predicate says occluded).
// Samples within a small |margin| of 0 are grazing (fp-ambiguous) and skipped in the oracle.
float ConeMargin(const Vector3& P, const Vector3& C, float r)
{
    const float pLen = P.Length(), cLen = C.Length();
    const float cosTheta = Vector3::Dot(P, C) / (pLen * cLen);
    const float cosC = r / cLen, sinC = std::sqrt(std::fmax(1.0f - cosC * cosC, 0.0f));
    const float cosP = std::fmin(std::fmax(r / pLen, -1.0f), 1.0f);
    const float sinP = std::sqrt(std::fmax(1.0f - cosP * cosP, 0.0f));
    return cosTheta - (cosC * cosP - sinC * sinP);
}

// A deterministic spread of unit directions across all six faces (avoids the axes so the
// tangent frame + dominant-face pick are unambiguous).
std::vector<Vector3> SampleDirections()
{
    std::vector<Vector3> dirs;
    for (int i = -5; i <= 5; ++i)
        for (int j = -5; j <= 5; ++j)
            for (int k = -5; k <= 5; ++k)
            {
                if (i == 0 && j == 0 && k == 0)
                    continue;
                dirs.push_back(Vector3(static_cast<float>(i) + 0.13f, static_cast<float>(j) + 0.29f,
                                       static_cast<float>(k) + 0.47f)
                                   .Normalize());
            }
    return dirs;
}
} // namespace

// The analytic 3D relief gradient agrees with a central finite difference of PlanetRelief at
// random directions. This is the core correctness of the "differentiate CBT_PlanetRelief
// analytically" requirement — a wrong partial (or a zero/facet gradient) fails.
TEST(CBTPlanetShading, ReliefGradientMatchesFiniteDifference)
{
    const float amp = 60.0f, freq = 6.0f;
    const float eps = 1e-4f;
    for (uint32_t oct = 1; oct <= 4; ++oct)
    {
        for (const Vector3& d : SampleDirections())
        {
            const Vector3 g = PlanetReliefGradient(d.x, d.y, d.z, amp, freq, oct);
            for (int axis = 0; axis < 3; ++axis)
            {
                Vector3 dp = d, dm = d;
                dp[axis] += eps;
                dm[axis] -= eps;
                const float fd = (PlanetRelief(dp.x, dp.y, dp.z, amp, freq, oct) -
                                  PlanetRelief(dm.x, dm.y, dm.z, amp, freq, oct)) /
                                 (2.0f * eps);
                // Tolerance scales with the frequency stack magnitude (higher octaves have
                // larger derivatives, hence larger absolute FD truncation error).
                EXPECT_NEAR(g[axis], fd, 0.5f + 0.02f * std::fabs(g[axis]))
                    << "oct=" << oct << " axis=" << axis;
            }
        }
    }
}

// The analytic shading normal matches the geometric normal from a finite-difference of the
// displaced surface. Validates the full N formula (gradient + the 1/(R+h) projection), and
// is discriminating: a radial/flat normal is off by the surface slope angle and fails.
TEST(CBTPlanetShading, AnalyticNormalMatchesGeometricNormal)
{
    const float radius = 2000.0f, amp = 60.0f, freq = 6.0f;
    const uint32_t oct = 3;
    const float eps = 5e-4f;
    float maxRadialAngleDeg = 0.0f;
    for (const Vector3& d : SampleDirections())
    {
        const Vector3 t1 = AnyTangent(d);
        const Vector3 t2 = Vector3::Cross(d, t1);
        const Vector3 pu = SurfacePoint(d + t1 * eps, radius, amp, freq, oct);
        const Vector3 mu = SurfacePoint(d - t1 * eps, radius, amp, freq, oct);
        const Vector3 pv = SurfacePoint(d + t2 * eps, radius, amp, freq, oct);
        const Vector3 mv = SurfacePoint(d - t2 * eps, radius, amp, freq, oct);
        Vector3 nGeo = Vector3::Cross(pu - mu, pv - mv).Normalize();
        if (Vector3::Dot(nGeo, d) < 0.0f)
            nGeo = nGeo * -1.0f;

        const Vector3 nA = PlanetShadingNormal(d, radius, amp, freq, oct);
        const float cosang = std::fmin(std::fmax(Vector3::Dot(nA, nGeo), -1.0f), 1.0f);
        const float angleDeg = ToDegrees(std::acos(cosang));
        EXPECT_LT(angleDeg, 1.5f) << "analytic vs geometric normal diverged";

        // Discriminating: where relief has slope, the normal is measurably off radial.
        const float radialCos = std::fmin(std::fmax(Vector3::Dot(nA, d), -1.0f), 1.0f);
        maxRadialAngleDeg = std::fmax(maxRadialAngleDeg, ToDegrees(std::acos(radialCos)));
    }
    EXPECT_GT(maxRadialAngleDeg, 3.0f)
        << "normal never departs from radial — a flat/facet normal would pass this by mistake";
}

// The shading normal depends only on world direction, so at a shared cube edge/corner it is
// identical whether the neighbourhood is parametrised through face A or face B. Sampling a
// direction ON an edge and re-deriving it through both adjacent faces' (u,v) must agree.
TEST(CBTPlanetShading, NormalSeamFreeAcrossCubeEdgesAndCorners)
{
    const float radius = 3000.0f, amp = 90.0f, freq = 8.0f;
    const uint32_t oct = 3;
    // Directions exactly on shared cube edges and corners (integer cube coords -> exact).
    const std::array<Vector3, 6> edgeAndCorner = {{
        Vector3(1.0f, 1.0f, 0.3f).Normalize(),  // +X/+Y edge region
        Vector3(1.0f, 0.0f, 1.0f).Normalize(),  // +X/+Z edge
        Vector3(0.2f, 1.0f, 1.0f).Normalize(),  // +Y/+Z edge
        Vector3(1.0f, 1.0f, 1.0f).Normalize(),  // +X/+Y/+Z corner
        Vector3(-1.0f, 1.0f, 1.0f).Normalize(), // -X/+Y/+Z corner
        Vector3(1.0f, -1.0f, 1.0f).Normalize(), // +X/-Y/+Z corner
    }};
    for (const Vector3& d : edgeAndCorner)
    {
        // The normal is the same object regardless of which face WorldDirToFaceUV picks;
        // re-project through EVERY face the direction lands on and confirm the normal is
        // unchanged (it never consults the face for the relief part).
        const Vector3 nRef = PlanetShadingNormal(d, radius, amp, freq, oct);
        for (uint32_t f = 0; f < kCubeFaceCount; ++f)
        {
            float u, v;
            if (!ProjectDirOntoFace(f, d.x, d.y, d.z, 1e-3f, u, v))
                continue;
            float rx, ry, rz;
            FaceUVToWorldDir(f, u, v, rx, ry, rz);
            const Vector3 n2 = PlanetShadingNormal({rx, ry, rz}, radius, amp, freq, oct);
            const float cosang = std::fmin(std::fmax(Vector3::Dot(nRef, n2), -1.0f), 1.0f);
            EXPECT_GT(cosang, 0.99999f) << "normal differs across face " << f << " at a shared edge";
        }
    }
}

// Multi-octave relief keeps the +/- 1.5*amplitude envelope for ANY octave count (so the
// on-shell bound in CBTSphereDomainTests stays valid), and adds FINE-SCALE detail: the
// shading normal wiggles more over a short arc as octaves increase (the higher frequencies
// contribute real high-frequency gradient content, which is the visible benefit). ampSum
// normalization bounds the amplitude, so the gain shows as fine light variation, not range.
TEST(CBTPlanetShading, MultiOctaveEnvelopeAndDetail)
{
    const float amp = 100.0f, freq = 6.0f, radius = 2000.0f;
    const float envelope = kReliefEnvelope * amp + 1e-3f;
    for (const Vector3& d : SampleDirections())
        for (uint32_t oct = 1; oct <= 4; ++oct)
        {
            const float h = PlanetRelief(d.x, d.y, d.z, amp, freq, oct);
            EXPECT_LE(std::fabs(h), envelope) << "relief exceeds the +/-1.5*amp envelope, oct=" << oct;
        }

    // Fine-scale normal roughness: sum the angle between consecutive shading normals along a
    // short tangent arc, sampled finely enough to resolve the highest octave (freq*8).
    auto normalRoughness = [&](uint32_t oct) {
        const Vector3 base = Vector3(0.7f, 0.5f, 0.4f).Normalize();
        const Vector3 t1 = AnyTangent(base);
        double total = 0.0;
        Vector3 prev{};
        for (int i = -200; i <= 200; ++i)
        {
            const float theta = static_cast<float>(i) * 0.0015f; // fine arc, ~0.6 rad total
            const Vector3 d = (base + t1 * theta).Normalize();
            const Vector3 n = PlanetShadingNormal(d, radius, amp, freq, oct);
            if (i > -200)
            {
                const float c = std::fmin(std::fmax(Vector3::Dot(n, prev), -1.0f), 1.0f);
                total += std::acos(c);
            }
            prev = n;
        }
        return total;
    };
    EXPECT_GT(normalRoughness(4), normalRoughness(1) * 1.5)
        << "adding octaves did not add fine-scale normal detail";
}

// The horizon-cull predicate is a CONSERVATIVE occlusion test — it must agree with the
// brute-force ray/floor-sphere ground truth over random cameras/points, and must NOT wrongly
// cull raised limb terrain the way a horizon-PLANE test does. Discriminating: it fails if the
// predicate is reverted to the plane test (which disagrees with brute-force here).
TEST(CBTPlanetShading, HorizonCullConservativeVsBruteForce)
{
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> elev(0.0f, 0.12f); // point radius above the floor
    std::uniform_real_distribution<float> camMul(1.2f, 6.0f);
    std::uniform_real_distribution<float> rDist(500.0f, 5000.0f);

    int clear = 0, planeWrong = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const float r = rDist(rng);
        const Vector3 pd = Vector3(unit(rng), unit(rng), unit(rng)).NormalizeOrZero();
        const Vector3 cd = Vector3(unit(rng), unit(rng), unit(rng)).NormalizeOrZero();
        if (pd.Length() < 0.1f || cd.Length() < 0.1f)
            continue;
        const Vector3 P = pd * (r * (1.0f + elev(rng)));
        const Vector3 C = cd * (r * camMul(rng));
        const bool bf = BruteForceOccluded(P, C, r);
        const bool cone = SphereCornerOccluded(P, C, r);
        if (std::fabs(ConeMargin(P, C, r)) > 2e-3f) // skip fp-grazing samples
        {
            EXPECT_EQ(cone, bf) << "cone predicate disagrees with brute-force occlusion";
            ++clear;
        }
        // The OLD horizon-plane test (dot(P,C) < r*|P|) is wrong somewhere -> the fix matters.
        const bool plane = Vector3::Dot(P, C) < r * P.Length();
        if (plane != bf)
            ++planeWrong;
    }
    EXPECT_GT(clear, 1000) << "too few clear samples to be meaningful";
    EXPECT_GT(planeWrong, 0) << "the horizon-plane test must disagree with truth (the bug)";

    // The reviewer's worked example: a limb peak just beyond the geometric horizon is VISIBLE.
    const Vector3 P{0.0f, 2090.0f, 0.0f};
    const Vector3 C{6000.0f, 0.0f, 0.0f};
    const float r = 1910.0f; // 2000 - 1.5*60
    EXPECT_FALSE(SphereCornerOccluded(P, C, r)) << "conservative test must keep the visible peak";
    EXPECT_TRUE(Vector3::Dot(P, C) < r * P.Length()) << "the plane test WOULD have wrongly culled it";
}

// ---------------------------------------------------------------------------
// RefinePlanetHit — the brush raycast lands on the DISPLACED surface, not the base sphere
// (#488 follow-up). The analytic RaycastPlanet solves |o + t d| = radius, ignoring relief +
// sculpt, so the cursor drifts off tall features; RefinePlanetHit closes the gap.
// ---------------------------------------------------------------------------
namespace
{
// Analytic nearest ray/sphere hit param, mirroring RaycastPlanet's base solve. d is unit.
bool BaseSphereHit(const Vector3& o, const Vector3& d, float radius, float& t)
{
    const float b = Vector3::Dot(o, d);
    const float c = Vector3::Dot(o, o) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f)
        return false;
    const float sq = std::sqrt(disc);
    t = -b - sq;
    if (t < 0.0f)
        t = -b + sq;
    return t >= 0.0f;
}
} // namespace

// A constant additive height inflates the surface to a sphere of radius + bump. The base hit
// sits on `radius`; the refined hit lands on `radius + bump` — a 150 m drift the fix removes.
TEST(RefinePlanetHit, ConstantHeightLandsOnInflatedSphere)
{
    const float radius = 2000.0f;
    const float bump = 150.0f;
    const Vector3 o{1800.0f, 0.0f, -6000.0f};       // grazing, hits near the limb
    const Vector3 d = Vector3(0.0f, 0.0f, 1.0f).Normalize();

    float t = 0.0f;
    ASSERT_TRUE(BaseSphereHit(o, d, radius, t));
    const Vector3 baseP = o + d * t;
    EXPECT_NEAR(baseP.Length(), radius, 0.5f);

    auto heightFn = [bump](float, float, float) { return bump; };
    ASSERT_TRUE(RefinePlanetHit(o, d, radius, heightFn, t));
    const Vector3 p = o + d * t;

    EXPECT_NEAR(p.Length(), radius + bump, 0.5f);                   // lands on the displaced surface
    EXPECT_GT(std::fabs(p.Length() - baseP.Length()), 100.0f);      // moved well off the base sphere
}

// With the REAL multi-octave relief the refined hit converges onto the CPU height-function
// surface (residual -> 0), and is never worse than the base-sphere hit's residual.
TEST(RefinePlanetHit, TracksReliefSurface)
{
    const float radius = 2000.0f;
    const float amp = 200.0f, freq = 6.0f;
    const uint32_t oct = 3u;
    const Vector3 o{1700.0f, 500.0f, -6000.0f};
    const Vector3 d = Vector3(0.0f, -0.02f, 1.0f).Normalize();

    float t = 0.0f;
    ASSERT_TRUE(BaseSphereHit(o, d, radius, t));
    const Vector3 baseP = o + d * t;
    const Vector3 baseDir = baseP.Normalize();
    const float baseResidual =
        std::fabs(baseP.Length() - (radius + PlanetRelief(baseDir.x, baseDir.y, baseDir.z, amp, freq, oct)));

    auto heightFn = [&](float dx, float dy, float dz) {
        return PlanetRelief(dx, dy, dz, amp, freq, oct);
    };
    ASSERT_TRUE(RefinePlanetHit(o, d, radius, heightFn, t));
    const Vector3 p = o + d * t;
    const Vector3 dir = p.Normalize();
    const float refinedResidual =
        std::fabs(p.Length() - (radius + PlanetRelief(dir.x, dir.y, dir.z, amp, freq, oct)));

    EXPECT_LT(refinedResidual, 0.5f);           // lands on the CPU height surface (the oracle)
    EXPECT_LE(refinedResidual, baseResidual);   // never worse than the un-refined base hit
}

// A non-positive radius is a no-op (leaves t untouched) — RaycastPlanet already rejected it.
TEST(RefinePlanetHit, RejectsNonPositiveRadius)
{
    const Vector3 o{0.0f, 0.0f, -100.0f};
    const Vector3 d = Vector3(0.0f, 0.0f, 1.0f).Normalize();
    float t = 42.0f;
    auto heightFn = [](float, float, float) { return 0.0f; };
    EXPECT_FALSE(RefinePlanetHit(o, d, 0.0f, heightFn, t));
    EXPECT_FLOAT_EQ(t, 42.0f);
}

// --- Untextured-fallback layer variation (round-8e: purple grass / cliff banding fixes) ----------
namespace
{
// A green grass tint and the default rock entry (Terrain::kDefaultTerrainMaterials, TerrainMaterialRecord.h)
// with their (scale, strength, saturation): a saturated green is where the purple-grass hue drift
// showed, so the variation is pinned on it rather than on the default grass channel.
constexpr ColorLinear kGrassTint(0.35f, 0.55f, 0.18f);
constexpr float kGrassScale = 0.030f, kGrassStrength = 0.16f, kGrassSat = 0.10f;
constexpr ColorLinear kRockTint(0.55f, 0.50f, 0.42f);
constexpr float kRockScale = 0.022f, kRockStrength = 0.24f, kRockSat = 0.12f;

// The offset of a color from the grey axis at its own luminance, as a direction in RGB space.
Vector3 Chroma(const ColorLinear& color)
{
    const float luminance = LinearRec709Luminance(color);
    return {color.r - luminance, color.g - luminance, color.b - luminance};
}

// Sample world positions on a 50 km planet surface (the round-8e scene shape) along a wall + a patch.
std::vector<Vector3> VariationSamplePositions()
{
    std::vector<Vector3> pts;
    const Vector3 dir = Vector3(0.3f, 0.9f, 0.32f).Normalize();
    for (int i = 0; i < 400; ++i)
    {
        // Walk 0.25 m steps in world Y (a vertical wall) plus a lateral spread, at R~50000.
        const float y = static_cast<float>(i) * 0.25f;
        const float lat = static_cast<float>(i % 40) * 0.7f;
        pts.push_back({dir.x * 50000.0f + lat, dir.y * 50000.0f + y, dir.z * 50000.0f});
    }
    return pts;
}
} // namespace

// The variation is HUE-PRESERVING: grass (green-dominant, G > R > B) never inverts its channel order,
// so it can never read purple/magenta (B rising above R/G) — the round-8e "purple grass" bug, which
// the old per-channel chroma jitter produced. Also checks the chroma DIRECTION (offset from the grey
// axis) stays parallel to the tint's, the crisp form of "hue unchanged".
TEST(CBTPlanetShading, LayerVariationPreservesHue)
{
    ASSERT_GT(kGrassTint.g, kGrassTint.r); // sanity: tint is green-dominant, blue-least
    ASSERT_GT(kGrassTint.r, kGrassTint.b);
    const Vector3 baseChroma = Chroma(kGrassTint);
    const float baseChromaLen = baseChroma.Length();
    for (const Vector3& p : VariationSamplePositions())
    {
        const ColorLinear c = PlanetLayerVariation(kGrassTint, p.x, p.y, p.z, kGrassScale, kGrassStrength,
                                                   kGrassSat, 0.0f);
        // Channel order preserved -> never purple.
        EXPECT_GE(c.g, c.r) << "grass lost green dominance (would read olive/other)";
        EXPECT_GE(c.r, c.b) << "grass blue rose above red -> purple cast (the pre-fix chroma bug)";
        EXPECT_EQ(c.a, kGrassTint.a) << "the variation must pass the tint's alpha through";
        // Chroma direction preserved (cosine ~ 1 with the tint's chroma).
        const Vector3 chroma = Chroma(c);
        const float len = chroma.Length();
        if (len > 1e-4f && baseChromaLen > 1e-4f)
        {
            const float cosSim = Vector3::Dot(chroma, baseChroma) / (len * baseChromaLen);
            EXPECT_GT(cosSim, 0.999f) << "variation shifted the hue direction";
        }
    }
}

// The variation Nyquist-fades to the flat tint as the pixel footprint grows past the noise cell:
// the deviation from the tint decreases monotonically to exactly zero (the distant-shimmer /
// grazing-streak fix). At footprint 0 it must still deviate (break-up retained — no #616 regression).
TEST(CBTPlanetShading, LayerVariationFadesToFlatTintWithFootprint)
{
    const Vector3 dir = Vector3(0.3f, 0.9f, 0.32f).Normalize();
    const Vector3 p = {dir.x * 50000.0f + 3.1f, dir.y * 50000.0f + 7.7f, dir.z * 50000.0f};
    auto deviation = [&](float footprint) {
        const ColorLinear c = PlanetLayerVariation(kRockTint, p.x, p.y, p.z, kRockScale, kRockStrength,
                                                   kRockSat, footprint);
        return std::fabs(c.r - kRockTint.r) + std::fabs(c.g - kRockTint.g) + std::fabs(c.b - kRockTint.b);
    };
    const float cell = 1.0f / kRockScale; // metres per noise cell
    const float d0 = deviation(0.0f);
    EXPECT_GT(d0, 0.01f) << "variation must still break up the flat tint at full detail (no regression)";
    // cellsPerPixel = footprint * scale; fade reaches 0 at cellsPerPixel = kVariationFadeHi.
    float prev = d0;
    for (float cpp = 0.1f; cpp <= 1.5f; cpp += 0.1f)
    {
        const float dev = deviation(cpp * cell);
        EXPECT_LE(dev, prev + 1e-4f) << "deviation must be monotonically non-increasing with footprint";
        prev = dev;
    }
    EXPECT_NEAR(deviation((kVariationFadeHi + 0.05f) * cell), 0.0f, 1e-5f)
        << "past the fade band the variation must be exactly the flat tint";
}

namespace
{
// PlanetRelief with the C library's sine in double of each float argument: the change of the sine is
// the only difference, as the float arguments and the octave sum are the same operations.
float ReferencePlanetRelief(const Vector3& d, float amplitude, float frequency, uint32_t octaves)
{
    const auto sine = [](float x) { return static_cast<float>(std::sin(static_cast<double>(x))); };
    float total = 0.0f, a = 1.0f, f = frequency, ampSum = 0.0f;
    for (uint32_t i = 0; i < octaves; ++i)
    {
        const float noise = sine(d.x * f) * sine(d.y * f * 1.3f + 0.7f) * sine(d.z * f * 0.7f + 1.9f) +
                            0.5f * sine(d.x * f * 2.1f + 1.3f) * sine(d.y * f * 1.7f + 2.4f);
        total += a * noise;
        ampSum += a;
        a *= kReliefPersistence;
        f *= kReliefLacunarity;
    }
    return amplitude * total / ampSum;
}

std::vector<Vector3> RandomUnitDirections(std::size_t count, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::vector<Vector3> directions(count);
    for (Vector3& d : directions)
    {
        d = {gauss(rng), gauss(rng), gauss(rng)};
        d = d * (1.0f / d.Length());
    }
    return directions;
}

// The largest change of PlanetRelief from ReferencePlanetRelief, as a fraction of the amplitude: 7.5 mm
// at 15 km (R/40 on a 600 km planet), about 8 float ULPs of 15 km.
constexpr double kReliefBound = 5.0e-7;
} // namespace

// The relief against the relief with the C library's sine: within kReliefBound x amplitude, 1 to 8 octaves.
TEST(CBTPlanetRelief, ReliefIsWithinItsBoundOfTheLibrarySineRelief)
{
    const std::vector<Vector3> directions = RandomUnitDirections(20000u, 6722u);
    constexpr float kAmplitude = 15000.0f; // R/40 on a 600 km planet
    double worst = 0.0;
    for (uint32_t octaves = 1; octaves <= 8u; ++octaves)
        for (const float frequency : {6.0f, 40.0f})
            for (const Vector3& d : directions)
            {
                const float h = PlanetRelief(d.x, d.y, d.z, kAmplitude, frequency, octaves);
                const float reference = ReferencePlanetRelief(d, kAmplitude, frequency, octaves);
                worst = std::max(worst, static_cast<double>(std::fabs(h - reference)));
            }
    RecordProperty("worstMeters", std::format("{:.3e}", worst));
    EXPECT_LE(worst, kReliefBound * kAmplitude);
}

// PlanetReliefRow has the bits of one PlanetRelief call per direction, for any row length, octave
// count, band limit, frequency and amplitude: the pages, collision and every CPU reader agree exactly.
TEST(CBTPlanetRelief, RowIsPlanetReliefBitForBit)
{
    const std::vector<Vector3> directions = RandomUnitDirections(1003u, 6808u);
    std::vector<float> row(directions.size());
    uint32_t mismatches = 0;
    for (const float frequency : {6.0f, 40.0f})
        for (const float amplitude : {0.0f, 60.0f, 15000.0f})
            for (const uint32_t octaves : {0u, 1u, 4u, 8u})
                for (const float minResolvedArc : {0.0f, 1.0e-3f, 0.05f})
                    for (const std::size_t length :
                         {std::size_t{0}, std::size_t{7}, std::size_t{138}, directions.size()})
                    {
                        const std::span<const Vector3> in(directions.data(), length);
                        PlanetReliefRow(in, amplitude, frequency, octaves, minResolvedArc,
                                        std::span<float>(row.data(), length));
                        for (std::size_t i = 0; i < length; ++i)
                        {
                            const float one = PlanetRelief(in[i].x, in[i].y, in[i].z, amplitude, frequency, octaves,
                                                           minResolvedArc);
                            mismatches += std::bit_cast<uint32_t>(one) != std::bit_cast<uint32_t>(row[i]) ? 1u : 0u;
                        }
                    }
    EXPECT_EQ(mismatches, 0u);
}

// The relief has these bits on every platform and compiler: the sine is polynomial, not the C library's.
TEST(CBTPlanetRelief, HasTheSameBitsOnEveryPlatform)
{
    const std::array<Vector3, 4> directions = {Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.6f, 0.8f},
                                               Vector3{-0.48f, 0.6f, -0.64f}, Vector3{0.36f, -0.48f, 0.8f}};
    constexpr std::array<uint32_t, 4> kExpected = {1135240416u, 1127544250u, 1130790054u, 1137267605u};
    for (std::size_t i = 0; i < directions.size(); ++i)
    {
        const Vector3& d = directions[i];
        const uint32_t bits = std::bit_cast<uint32_t>(PlanetRelief(d.x, d.y, d.z, 1250.0f, 6.0f, 4u));
        RecordProperty("bits" + std::to_string(i), std::to_string(bits));
        EXPECT_EQ(bits, kExpected[i]) << "direction " << i;
    }
}
