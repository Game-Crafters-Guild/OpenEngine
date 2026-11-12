#pragma once

// The CPU reference of the planet relief (CBT_PlanetRelief) and its analytic normal. Edit the GLSL twins in
// cbt_domain.glsl with these. The relief stays inside +/- kReliefEnvelope x amplitude for any octave count.
// The CPU sine is Math::FastSin; the GPU uses its own sin, so the two agree to the GPU sin's error.

#include "CBTTerrain/CBTLayout.h"        // page constants
#include "CBTTerrain/CBTSphereFaceMap.h"  // WorldDirToFaceUV (dir -> face,uv)
#include "Mathematics/Transcendental.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>

namespace GameEngine::CBTTerrain
{

// fBM stack shape (mirror kReliefPersistence / kReliefLacunarity in cbt_domain.glsl).
// Persistence halves the amplitude and lacunarity doubles the frequency per octave — the
// standard fBM progression. The per-octave base noise envelope is [-kReliefEnvelope,
// +kReliefEnvelope]; normalizing the octave sum by its amplitude sum keeps the TOTAL relief
// inside that same envelope for any octave count (so a deeper stack adds detail, not range).
inline constexpr float kReliefPersistence = 0.5f;
// Non-harmonic (irrational) lacunarity so octave frequencies are not integer multiples of the
// base — the axis-separable sin-product octaves at 2x/4x harmonics reinforce on a regular grid
// (a visible lattice on the sphere); 2.13 never re-aligns. Mirror of CBT_RELIEF_LACUNARITY.
inline constexpr float kReliefLacunarity = 2.13f;
inline constexpr float kReliefEnvelope = 1.5f; // max |base noise| = 1.0 + 0.5 (its two terms)

// Single-octave base noise at direction (dx,dy,dz) and angular frequency f. The two sin
// products are the pre-multi-octave CBT_PlanetRelief kernel; the envelope is [-1.5, 1.5].
inline float PlanetBaseNoise(float dx, float dy, float dz, float f)
{
    const float s1 = Math::FastSin(dx * f);
    const float s2 = Math::FastSin(dy * f * 1.3f + 0.7f);
    const float s3 = Math::FastSin(dz * f * 0.7f + 1.9f);
    const float s4 = Math::FastSin(dx * f * 2.1f + 1.3f);
    const float s5 = Math::FastSin(dy * f * 1.7f + 2.4f);
    return s1 * s2 * s3 + 0.5f * s4 * s5;
}

// Gradient of PlanetBaseNoise w.r.t. (dx,dy,dz) — the closed-form partials of the two sin
// products (product rule). Used by the analytic surface normal.
inline Mathematics::Vector3 PlanetBaseNoiseGradient(float dx, float dy, float dz, float f)
{
    const float s1 = Math::FastSin(dx * f), c1 = Math::FastCos(dx * f);
    const float s2 = Math::FastSin(dy * f * 1.3f + 0.7f), c2 = Math::FastCos(dy * f * 1.3f + 0.7f);
    const float s3 = Math::FastSin(dz * f * 0.7f + 1.9f), c3 = Math::FastCos(dz * f * 0.7f + 1.9f);
    const float s4 = Math::FastSin(dx * f * 2.1f + 1.3f), c4 = Math::FastCos(dx * f * 2.1f + 1.3f);
    const float s5 = Math::FastSin(dy * f * 1.7f + 2.4f), c5 = Math::FastCos(dy * f * 1.7f + 2.4f);
    const float gx = f * c1 * s2 * s3 + 0.5f * (2.1f * f) * c4 * s5;
    const float gy = (1.3f * f) * s1 * c2 * s3 + 0.5f * (1.7f * f) * s4 * c5;
    const float gz = (0.7f * f) * s1 * s2 * c3;
    return {gx, gy, gz};
}

// The highest angular frequency in PlanetBaseNoise, as a multiple of its f (the s4 term).
inline constexpr float kReliefHighestFrequencyFactor = 2.1f;
inline constexpr float kReliefTwoPi = 2.0f * std::numbers::pi_v<float>;

// Multi-octave (fBM) relief in metres at direction (dx,dy,dz), inside +/- kReliefEnvelope x amplitude
// for any octave count (0 counts as 1). An octave whose shortest wavelength on the unit sphere is
// under minResolvedArc radians adds its mean, 0; minResolvedArc 0 evaluates every octave.
inline float PlanetRelief(float dx, float dy, float dz, float amplitude, float frequency,
                          uint32_t octaves, float minResolvedArc = 0.0f)
{
    if (amplitude <= 0.0f)
        return 0.0f;
    const uint32_t oct = octaves < 1u ? 1u : octaves;
    float total = 0.0f, a = 1.0f, f = frequency, amplitudeSum = 0.0f;
    for (uint32_t i = 0; i < oct; ++i)
    {
        if (kReliefHighestFrequencyFactor * f * minResolvedArc <= kReliefTwoPi)
            total += a * PlanetBaseNoise(dx, dy, dz, f);
        amplitudeSum += a;
        a *= kReliefPersistence;
        f *= kReliefLacunarity;
    }
    return amplitude * total / amplitudeSum;
}

// PlanetRelief at each of `directions` into `outHeights` (the same size). Each height has the bits of
// one PlanetRelief call at that direction; eight directions go through one step where AVX2 is on.
void PlanetReliefRow(std::span<const Mathematics::Vector3> directions, float amplitude, float frequency,
                     uint32_t octaves, float minResolvedArc, std::span<float> outHeights);

// An upper bound of |gradient| of PlanetBaseNoise, as a multiple of its f: the norm of the per-axis
// bounds of PlanetBaseNoiseGradient (2.05, 2.15, 0.7), rounded up. The three never peak together.
inline constexpr float kPlanetBaseNoiseSlope = 3.06f;

// The rounding allowance of PlanetReliefRange, as a fraction of the amplitude.
inline constexpr float kPlanetReliefRangeRounding = 1.0e-4f;

// The lowest and highest relief in metres over a region of directions.
struct PlanetReliefRange
{
    float Min = 0.0f;
    float Max = 0.0f;
};

// The range of PlanetRelief (CPU only) within `chord`, a straight-line distance, of unit direction
// (dx,dy,dz), at `minResolvedArc` and at 0. Conservative: an octave changes by at most
// kPlanetBaseNoiseSlope x f x chord inside its envelope; an octave dropped at minResolvedArc adds 0.
inline PlanetReliefRange PlanetReliefRangeNear(float dx, float dy, float dz, float chord, float amplitude,
                                               float frequency, uint32_t octaves, float minResolvedArc)
{
    if (amplitude <= 0.0f)
        return {};
    const uint32_t oct = octaves < 1u ? 1u : octaves;
    float low = 0.0f, high = 0.0f, a = 1.0f, f = frequency, amplitudeSum = 0.0f;
    for (uint32_t i = 0; i < oct; ++i)
    {
        const float value = PlanetBaseNoise(dx, dy, dz, f);
        const float reach = kPlanetBaseNoiseSlope * f * chord;
        float octaveLow = std::fmax(value - reach, -kReliefEnvelope);
        float octaveHigh = std::fmin(value + reach, kReliefEnvelope);
        if (kReliefHighestFrequencyFactor * f * minResolvedArc > kReliefTwoPi)
        {
            octaveLow = std::fmin(octaveLow, 0.0f);
            octaveHigh = std::fmax(octaveHigh, 0.0f);
        }
        low += a * octaveLow;
        high += a * octaveHigh;
        amplitudeSum += a;
        a *= kReliefPersistence;
        f *= kReliefLacunarity;
    }
    const float rounding = kPlanetReliefRangeRounding * amplitude;
    return {amplitude * low / amplitudeSum - rounding, amplitude * high / amplitudeSum + rounding};
}

// The 3D gradient d(relief)/d(dir) of the multi-octave relief. Project out the radial
// component to get the tangential (surface) gradient used by the shading normal.
inline Mathematics::Vector3 PlanetReliefGradient(float dx, float dy, float dz, float amplitude,
                                                 float frequency, uint32_t octaves)
{
    if (amplitude <= 0.0f)
        return {0.0f, 0.0f, 0.0f};
    const uint32_t oct = octaves < 1u ? 1u : octaves;
    float gx = 0.0f, gy = 0.0f, gz = 0.0f, a = 1.0f, f = frequency, amplitudeSum = 0.0f;
    for (uint32_t i = 0; i < oct; ++i)
    {
        const Mathematics::Vector3 g = PlanetBaseNoiseGradient(dx, dy, dz, f);
        gx += a * g.x;
        gy += a * g.y;
        gz += a * g.z;
        amplitudeSum += a;
        a *= kReliefPersistence;
        f *= kReliefLacunarity;
    }
    const float k = amplitude / amplitudeSum;
    return {gx * k, gy * k, gz * k};
}

// Conservative sphere occlusion (cbt_kernels.comp CBT_CornerOccluded mirror) for the horizon
// cull. Is corner P (a displaced sphere point, |P| >= r) occluded by the floor-sphere of
// radius r as seen from camera C? Exact cone test: occluded iff the angular distance between P
// and C exceeds thetaC + thetaP, cos(thetaC)=r/|C|, cos(thetaP)=r/|P|. r is the surface FLOOR
// (radius minus the max relief drop), so no visible limb terrain is ever culled — unlike a
// horizon-PLANE test (dot(P,C) < r|P|), which culls raised terrain beyond the geometric
// horizon that is still visible. Camera at/under the floor culls nothing.
inline bool SphereCornerOccluded(const Mathematics::Vector3& P, const Mathematics::Vector3& C, float r)
{
    const float pLen = std::sqrt(P.x * P.x + P.y * P.y + P.z * P.z);
    const float cLen = std::sqrt(C.x * C.x + C.y * C.y + C.z * C.z);
    if (pLen <= 0.0f || cLen <= r)
        return false;
    const float cosTheta = (P.x * C.x + P.y * C.y + P.z * C.z) / (pLen * cLen);
    const float cosC = r / cLen;
    const float sinC = std::sqrt(std::fmax(1.0f - cosC * cosC, 0.0f));
    const float cosP = std::fmin(std::fmax(r / pLen, -1.0f), 1.0f);
    const float sinP = std::sqrt(std::fmax(1.0f - cosP * cosP, 0.0f));
    return cosTheta < cosC * cosP - sinC * sinP;
}

// Refine a ray->planet hit from the analytic BASE sphere to the DISPLACED surface
// (radius + height(dir)). RaycastPlanet solves |o + t d| = radius, which ignores relief +
// sculpt, so the brush cursor drifts off tall features (#488). Fixed-point iteration closes
// the gap: at each step estimate the local surface radius R + h(dir(t)), re-solve the
// nearest ray-sphere intersection at that radius, and update t. Because the height is << R
// the per-step direction correction is small, so this converges in a few iterations. On the
// fixed point |o + t d| == radius + h(dir), i.e. the hit lies exactly on the displaced
// surface. heightFn(dx,dy,dz) returns metres above `radius` for a (not necessarily unit)
// direction. `t` is the base-sphere hit on input and the refined hit on output. `d` must be
// unit. Returns false for a non-positive radius (leaving `t` untouched).
template <typename HeightFn>
inline bool RefinePlanetHit(const Mathematics::Vector3& o, const Mathematics::Vector3& d,
                            float radius, HeightFn&& heightFn, float& t)
{
    if (radius <= 0.0f)
        return false;
    constexpr uint32_t kRefineIters = 6u;
    const float b = Mathematics::Vector3::Dot(o, d);
    const float oo = Mathematics::Vector3::Dot(o, o);
    for (uint32_t i = 0; i < kRefineIters; ++i)
    {
        const float px = o.x + d.x * t, py = o.y + d.y * t, pz = o.z + d.z * t;
        const float pLen = std::sqrt(px * px + py * py + pz * pz);
        if (pLen <= 0.0f)
            break;
        const float inv = 1.0f / pLen;
        const float r = radius + heightFn(px * inv, py * inv, pz * inv);
        // Nearest non-negative root of |o + t d|^2 = r^2.
        const float disc = b * b - (oo - r * r);
        if (disc < 0.0f)
            break; // the corrected sphere is no longer reached; keep the last t
        const float sq = std::sqrt(disc);
        float tNew = -b - sq;
        if (tNew < 0.0f)
            tNew = -b + sq; // origin inside the corrected sphere: take the far root
        if (tNew < 0.0f)
            break;
        t = tNew;
    }
    return true;
}

// The outward analytic shading normal of the relief-displaced sphere at UNIT direction
// `dir` (planet centred at the world origin, radius R). h is the total height offset above
// R (relief here; the GPU adds the sculpt sample); surfaceGrad is the TANGENTIAL gradient of
// that offset (project the 3D gradient: g - (g.dir) dir). Derivation (cbt_domain.glsl):
// the surface tangents are t*(R+h) + dir*(dh/dt), whose cross product normalizes to
// dir - surfaceGrad/(R+h). Reduces to `dir` when the surface is smooth (grad 0).
inline Mathematics::Vector3 SphereShadingNormalFromGrad(const Mathematics::Vector3& dir, float radius,
                                                        float heightOffset,
                                                        const Mathematics::Vector3& surfaceGrad)
{
    const float invScale = 1.0f / (radius + heightOffset);
    float nx = dir.x - surfaceGrad.x * invScale;
    float ny = dir.y - surfaceGrad.y * invScale;
    float nz = dir.z - surfaceGrad.z * invScale;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {nx * inv, ny * inv, nz * inv};
}

// Convenience: the relief-only analytic normal at unit direction `dir`. Projects the relief
// gradient to the tangent plane and applies SphereShadingNormalFromGrad.
inline Mathematics::Vector3 PlanetShadingNormal(const Mathematics::Vector3& dir, float radius,
                                                float amplitude, float frequency, uint32_t octaves)
{
    const float h = PlanetRelief(dir.x, dir.y, dir.z, amplitude, frequency, octaves);
    const Mathematics::Vector3 g = PlanetReliefGradient(dir.x, dir.y, dir.z, amplitude, frequency, octaves);
    const float gd = g.x * dir.x + g.y * dir.y + g.z * dir.z;
    const Mathematics::Vector3 gs = {g.x - gd * dir.x, g.y - gd * dir.y, g.z - gd * dir.z};
    return SphereShadingNormalFromGrad(dir, radius, h, gs);
}

// A unit tangent to `dir` (any consistent choice): cross with the world axis of smallest
// component magnitude, which is never parallel to dir.
inline Mathematics::Vector3 AnyTangent(const Mathematics::Vector3& dir)
{
    const float ax = std::fabs(dir.x), ay = std::fabs(dir.y), az = std::fabs(dir.z);
    const Mathematics::Vector3 up = (ax <= ay && ax <= az) ? Mathematics::Vector3{1.0f, 0.0f, 0.0f}
                                    : (ay <= az)           ? Mathematics::Vector3{0.0f, 1.0f, 0.0f}
                                                           : Mathematics::Vector3{0.0f, 0.0f, 1.0f};
    const Mathematics::Vector3 t = {dir.y * up.z - dir.z * up.y, dir.z * up.x - dir.x * up.z,
                                    dir.x * up.y - dir.y * up.x};
    const float len = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {t.x * inv, t.y * inv, t.z * inv};
}

// ---------------------------------------------------------------------------
// Untextured-fallback material variation
// (Includes/terrain_material_albedo.glsl CBT_ApplyMaterialVariation mirror)
// ---------------------------------------------------------------------------
// Hue-preserving value-noise variation that breaks up the flat layer tint (the "purple grass" /
// "terraced cliff band" fixes, round-8e). Kept in lockstep with the GLSL: the fragment reads this same
// math for an untextured planet layer, and the oracles below assert its two invariants headlessly —
// (1) it never shifts hue (channel ORDER of the tint is preserved: grass stays green-dominant, never
// purple), and (2) it Nyquist-fades to the flat tint as the pixel footprint grows past the noise cell.
inline float VariationFract(float x) { return x - std::floor(x); }

// hash31 (Includes/terrain_material_albedo.glsl CBT_Hash31 mirror).
inline float VariationHash31(float px, float py, float pz)
{
    px = VariationFract(px * 0.1031f);
    py = VariationFract(py * 0.1031f);
    pz = VariationFract(pz * 0.1031f);
    const float d = px * (pz + 31.32f) + py * (py + 31.32f) + pz * (px + 31.32f); // dot(p, p.zyx+31.32)
    px += d;
    py += d;
    pz += d;
    return VariationFract((px + py) * pz);
}

// Trilinear value noise (Includes/terrain_material_albedo.glsl CBT_ValueNoise mirror).
inline float VariationValueNoise(float px, float py, float pz)
{
    const float ix = std::floor(px), iy = std::floor(py), iz = std::floor(pz);
    const float fx = px - ix, fy = py - iy, fz = pz - iz;
    auto smooth = [](float f) { return f * f * (3.0f - 2.0f * f); };
    const float ux = smooth(fx), uy = smooth(fy), uz = smooth(fz);
    auto h = [&](float dx, float dy, float dz) { return VariationHash31(ix + dx, iy + dy, iz + dz); };
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = mix(h(0, 0, 0), h(1, 0, 0), ux), x10 = mix(h(0, 1, 0), h(1, 1, 0), ux);
    const float x01 = mix(h(0, 0, 1), h(1, 0, 1), ux), x11 = mix(h(0, 1, 1), h(1, 1, 1), ux);
    return mix(mix(x00, x10, uy), mix(x01, x11, uy), uz);
}

// Fade band + lattice rotation — mirror kVariationFade{Lo,Hi} / kVariationRot in
// Includes/terrain_material_albedo.glsl.
inline constexpr float kVariationFadeLo = 0.35f;
inline constexpr float kVariationFadeHi = 1.2f;
inline Mathematics::Vector3 VariationRotate(float x, float y, float z)
{
    // Columns of kVariationRot (GLSL column-major mat3): out = col0*x + col1*y + col2*z.
    return {0.64864f * x - 0.57400f * y + 0.49979f * z, 0.68211f * x + 0.72972f * y - 0.04719f * z,
            -0.33762f * x + 0.37152f * y + 0.86486f * z};
}

// The composed layer color (Includes/terrain_material_albedo.glsl CBT_ApplyMaterialVariation
// mirror). base is the flat tint; its alpha passes through.
// footprintWS the world-space pixel footprint (m); scale/strength/hue the per-layer variation params.
inline ColorLinear PlanetLayerVariation(const ColorLinear& base, float px, float py, float pz, float scale,
                                        float strength, float hue, float footprintWS)
{
    if (scale <= 0.0f || (strength <= 0.0f && hue <= 0.0f))
        return base;
    const float cellsPerPixel = footprintWS * scale;
    const float t = std::fmin(std::fmax((cellsPerPixel - kVariationFadeLo) /
                                            (kVariationFadeHi - kVariationFadeLo), 0.0f), 1.0f);
    const float fade = 1.0f - t * t * (3.0f - 2.0f * t); // 1 - smoothstep
    if (fade <= 0.0f)
        return base;
    const Mathematics::Vector3 p = VariationRotate(px * scale, py * scale, pz * scale);
    const float n = (VariationValueNoise(p.x, p.y, p.z) +
                     0.5f * VariationValueNoise(p.x * 4.17f + 7.3f, p.y * 4.17f + 7.3f, p.z * 4.17f + 7.3f) +
                     0.25f * VariationValueNoise(p.x * 17.4f + 19.1f, p.y * 17.4f + 19.1f, p.z * 17.4f + 19.1f)) *
                    (1.0f / 1.75f);
    const float brightness = 1.0f + fade * strength * (n * 2.0f - 1.0f);
    const ColorLinear col(base.r * brightness, base.g * brightness, base.b * brightness, base.a);
    const float nc = VariationValueNoise(p.x * 1.7f + 11.3f, p.y * 1.7f + 11.3f, p.z * 1.7f + 11.3f);
    const float sat = 1.0f + fade * hue * (nc * 2.0f - 1.0f);
    const float lum = col.r * 0.2126f + col.g * 0.7152f + col.b * 0.0722f;
    return ColorLinear(std::fmax(lum + (col.r - lum) * sat, 0.0f), std::fmax(lum + (col.g - lum) * sat, 0.0f),
                       std::fmax(lum + (col.b - lum) * sat, 0.0f), base.a);
}

} // namespace GameEngine::CBTTerrain
