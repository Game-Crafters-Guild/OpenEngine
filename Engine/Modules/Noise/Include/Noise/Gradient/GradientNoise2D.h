#pragma once

#include "Mathematics/MulAdd.h"
#include "Types/Types.h"

#include <cmath>

// ---- The shared CPU noise module --------------------------------------------
//
// The engine's reusable noise primitives, in one place so that no feature grows
// a private copy. What lives here is generic: a sample is a pure function of its
// coordinates and seed, with no world, asset or component types in the surface.
//
// Present: the gradient-noise lattice below (hash, gradient table, value and
// analytic derivatives), the 3D gradient noise on an exact lattice in
// Gradient/GradientNoise3D.h, the curl of 3D gradient noise in
// Gradient/GradientCurl3D.h, and the 2D and 3D fractal sums in FractalNoise.h.
//
// Deliberately NOT here, because each is pinned bit-for-bit to its own GPU
// mirror and folding it in would change what that mirror is compared against:
//   - the planet relief fBM and its gradient (CBTTerrain/CBTPlanetShading.h,
//     mirror of cbt_domain.glsl)
//   - the albedo variation value noise (same header, mirror of
//     terrain_material_albedo.glsl)
//   - the heightfield base-fill value noise (Terrain/Source/Heightfield.cpp
//     NoiseHash/SmoothNoise, mirror of terrain_height_bake.comp BaseSmoothNoise)
// Also not here, for other reasons: the ocean caustics texture generator
// (Modules/Ocean/Source/OceanCaustics.cpp) is a tileable Worley edge field and
// belongs here once the module has cellular noise; the ocean extraction
// placement hash is a scalar sin hash, not a lattice; and the editor's
// interleaved-gradient dither is an ordered dither pattern rather than a noise
// field.
//
// A second implementation of anything in this module is a defect. Adding a new
// noise kind here is welcome; moving one of the mirrored fields above is not,
// unless its mirror moves with it.

namespace GameEngine::Noise
{

/// @brief One lattice corner's gradient direction, as the signs of a diagonal.
///
/// The four gradients are the diagonals (±1, ±1). Holding them as signs rather
/// than a computed value lets the value and derivative paths share one table:
/// a corner's contribution is SignX * dx + SignZ * dz, and the signs are also
/// that contribution's partial derivatives.
struct GradientDirection
{
    float32 SignX;
    float32 SignZ;
};

/// @brief A noise sample with its analytic gradient.
///
/// The derivatives are per unit of the x/z the caller passed, in the same units
/// as Value.
struct NoiseSample
{
    float32 Value = 0.0f;
    float32 DValueDX = 0.0f;
    float32 DValueDZ = 0.0f;
};

/// @brief Hashes an integer lattice cell to a well-mixed 32-bit value.
///
/// Expression order is part of the contract: GPU mirrors of this lattice
/// reproduce it term for term, so reassociating it changes what they have to
/// say.
inline uint32 LatticeHash(int32 ix, int32 iz, uint32 seed)
{
    uint32 h = static_cast<uint32>(ix) * 374761393u +
               static_cast<uint32>(iz) * 668265263u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/// @brief Selects one of the four diagonal gradients from a lattice hash.
inline GradientDirection CornerGradient(uint32 h)
{
    switch (h & 3u)
    {
    case 0u: return { 1.0f,  1.0f};
    case 1u: return {-1.0f,  1.0f};
    case 2u: return { 1.0f, -1.0f};
    default: return {-1.0f, -1.0f};
    }
}

/// @brief Perlin-like gradient noise over a hashed integer lattice.
///
/// Four diagonal gradients, smoothstep-interpolated. Output is roughly in
/// [-1, 1]. Expression order is part of the contract — the GPU mirrors
/// reproduce it term for term.
inline float32 GradientNoise2D(float32 x, float32 z, uint32 seed)
{
    const int32 ix = static_cast<int32>(std::floor(x));
    const int32 iz = static_cast<int32>(std::floor(z));
    const float32 fx = x - static_cast<float32>(ix);
    const float32 fz = z - static_cast<float32>(iz);

    const float32 u = fx * fx * Math::MulAdd(-2.0f, fx, 3.0f);
    const float32 v = fz * fz * Math::MulAdd(-2.0f, fz, 3.0f);

    const GradientDirection g00 = CornerGradient(LatticeHash(ix, iz, seed));
    const GradientDirection g10 = CornerGradient(LatticeHash(ix + 1, iz, seed));
    const GradientDirection g01 = CornerGradient(LatticeHash(ix, iz + 1, seed));
    const GradientDirection g11 = CornerGradient(LatticeHash(ix + 1, iz + 1, seed));

    const float32 n00 = Math::MulAdd(g00.SignX, fx,          g00.SignZ * fz);
    const float32 n10 = Math::MulAdd(g10.SignX, fx - 1.0f,   g10.SignZ * fz);
    const float32 n01 = Math::MulAdd(g01.SignX, fx,          g01.SignZ * (fz - 1.0f));
    const float32 n11 = Math::MulAdd(g11.SignX, fx - 1.0f,   g11.SignZ * (fz - 1.0f));

    const float32 nx0 = Math::MulAdd(u, n10 - n00, n00);
    const float32 nx1 = Math::MulAdd(u, n11 - n01, n01);
    return Math::MulAdd(v, nx1 - nx0, nx0);
}

/// @brief GradientNoise2D with its analytic gradient.
///
/// The value path is written in the same expression order as GradientNoise2D so
/// the two agree bit for bit; the derivatives differentiate that same smoothstep
/// interpolation, so no finite differencing and no neighbouring sample is needed.
inline NoiseSample GradientNoise2DWithDerivatives(float32 x, float32 z, uint32 seed)
{
    const int32 ix = static_cast<int32>(std::floor(x));
    const int32 iz = static_cast<int32>(std::floor(z));
    const float32 fx = x - static_cast<float32>(ix);
    const float32 fz = z - static_cast<float32>(iz);

    const GradientDirection g00 = CornerGradient(LatticeHash(ix, iz, seed));
    const GradientDirection g10 = CornerGradient(LatticeHash(ix + 1, iz, seed));
    const GradientDirection g01 = CornerGradient(LatticeHash(ix, iz + 1, seed));
    const GradientDirection g11 = CornerGradient(LatticeHash(ix + 1, iz + 1, seed));

    const float32 n00 = Math::MulAdd(g00.SignX, fx,          g00.SignZ * fz);
    const float32 n10 = Math::MulAdd(g10.SignX, fx - 1.0f,   g10.SignZ * fz);
    const float32 n01 = Math::MulAdd(g01.SignX, fx,          g01.SignZ * (fz - 1.0f));
    const float32 n11 = Math::MulAdd(g11.SignX, fx - 1.0f,   g11.SignZ * (fz - 1.0f));

    const float32 u = fx * fx * Math::MulAdd(-2.0f, fx, 3.0f);
    const float32 v = fz * fz * Math::MulAdd(-2.0f, fz, 3.0f);
    const float32 du = 6.0f * fx * (1.0f - fx);
    const float32 dv = 6.0f * fz * (1.0f - fz);

    const float32 nx0 = Math::MulAdd(u, n10 - n00, n00);
    const float32 nx1 = Math::MulAdd(u, n11 - n01, n01);

    // d/dx of each row, then across: the interpolation weight moves too, so the
    // chain rule contributes du * (row difference).
    const float32 dx0 = Math::MulAdd(du, n10 - n00, Math::MulAdd(u, g10.SignX - g00.SignX, g00.SignX));
    const float32 dx1 = Math::MulAdd(du, n11 - n01, Math::MulAdd(u, g11.SignX - g01.SignX, g01.SignX));
    const float32 dz0 = Math::MulAdd(u, g10.SignZ - g00.SignZ, g00.SignZ);
    const float32 dz1 = Math::MulAdd(u, g11.SignZ - g01.SignZ, g01.SignZ);

    NoiseSample out{};
    out.Value = Math::MulAdd(v, nx1 - nx0, nx0);
    out.DValueDX = Math::MulAdd(v, dx1 - dx0, dx0);
    out.DValueDZ = Math::MulAdd(dv, nx1 - nx0, Math::MulAdd(v, dz1 - dz0, dz0));
    return out;
}

} // namespace GameEngine::Noise
