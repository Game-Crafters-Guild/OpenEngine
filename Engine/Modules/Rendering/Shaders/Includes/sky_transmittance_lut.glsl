#ifndef GE_SKY_TRANSMITTANCE_LUT_GLSL
#define GE_SKY_TRANSMITTANCE_LUT_GLSL

// The transmittance LUT's parameterisation, shared by the pass that bakes it
// (sky_transmittance_lut.comp) and every pass that reads it.
//
// The LUT covers only the rays that reach the top of the atmosphere without meeting the planet:
// Bruneton 2017's mapping (precomputed_atmospheric_scattering, GetTransmittanceTextureUvFromRMu),
// which Hillaire 2020 uses too. With H = sqrt(top^2 - bottom^2) the length of the horizontal ray
// from the ground to the top, rho = sqrt(r^2 - bottom^2) the same at radius r, and d the distance
// from (r, mu) to the top boundary:
//   x_r  = rho / H
//   x_mu = (d - d_min) / (d_max - d_min),  d_min = top - r (straight up), d_max = rho + H (the
//          ray grazing the planet)
// so the horizon is the LUT's edge, x_mu = 1, and a bilinear read at any above-horizon mu mixes
// two above-horizon texels. Each unit coordinate is inset by half a texel (Bruneton's
// GetTextureCoordFromUnitRange) so x = 0 and x = 1 land on the edge texel centres.
//
// A ray that meets the planet is not in the LUT: the planet occludes it, and
// GE_SkyTransmittanceToTop returns 0 for it. That is the planet's shadow (Hillaire 2020's
// earthShadow) for every pass that asks how much sunlight reaches a point.
//
// Every difference of two planet-scale squares is factored as (a - b) * (a + b), for the reason
// sky_ray_sphere.glsl states.

#include "sky_ray_sphere.glsl"

float GE_SkyLutUnitToTexCoord(float x, float texels)
{
    return 0.5 / texels + x * (1.0 - 1.0 / texels);
}

float GE_SkyLutTexCoordToUnit(float u, float texels)
{
    return (u - 0.5 / texels) / (1.0 - 1.0 / texels);
}

// True when the ray from radius r along zenith cosine mu meets the planet (bottom) before it
// leaves the atmosphere: mu < 0 and r^2 (1 - mu^2) < bottom^2.
bool GE_SkyRayHitsGround(float r, float mu, float bottom)
{
    float rMu = r * mu;
    return mu < 0.0 && rMu * rMu - (r - bottom) * (r + bottom) >= 0.0;
}

vec2 GE_SkyTransmittanceLutRMuToUv(float r, float mu, float bottom, float top, vec2 lutSize)
{
    float H = sqrt((top - bottom) * (top + bottom));
    float rho = sqrt(max((r - bottom) * (r + bottom), 0.0));
    float d = GE_DistanceToOuterSphere(r, mu, top);
    float dMin = top - r;
    float dMax = rho + H;
    float xMu = clamp((d - dMin) / max(dMax - dMin, 1e-3), 0.0, 1.0);
    float xR = clamp(rho / H, 0.0, 1.0);
    return vec2(GE_SkyLutUnitToTexCoord(xMu, lutSize.x), GE_SkyLutUnitToTexCoord(xR, lutSize.y));
}

// Inverse of GE_SkyTransmittanceLutRMuToUv: the (r, mu) a texel centre stands for.
vec2 GE_SkyTransmittanceLutUvToRMu(vec2 uv, float bottom, float top, vec2 lutSize)
{
    float xMu = GE_SkyLutTexCoordToUnit(uv.x, lutSize.x);
    float xR = GE_SkyLutTexCoordToUnit(uv.y, lutSize.y);
    float H = sqrt((top - bottom) * (top + bottom));
    float rho = H * xR;
    float r = sqrt(rho * rho + bottom * bottom);
    float dMin = top - r;
    float dMax = rho + H;
    float d = dMin + xMu * (dMax - dMin);
    // Law of cosines on the triangle (centre, point, exit): top^2 = r^2 + d^2 + 2 r d mu.
    float mu = (d <= 0.0) ? 1.0 : ((top - r) * (top + r) - d * d) / (2.0 * r * d);
    return vec2(r, clamp(mu, -1.0, 1.0));
}

// Transmittance from radius r along zenith cosine mu to the top of the atmosphere; 0 when the
// planet is in the way. r is clamped into the shell: above the atmosphere nothing attenuates the
// part of the path the LUT does not cover, and below the ground the surface row is the nearest
// the LUT holds.
vec3 GE_SkyTransmittanceToTop(sampler2D lut, float r, float mu, float bottom, float top)
{
    float rc = clamp(r, bottom, top);
    if (GE_SkyRayHitsGround(rc, mu, bottom))
        return vec3(0.0);
    vec2 lutSize = vec2(textureSize(lut, 0));
    return textureLod(lut, GE_SkyTransmittanceLutRMuToUv(rc, mu, bottom, top, lutSize), 0.0).rgb;
}

#endif // GE_SKY_TRANSMITTANCE_LUT_GLSL
