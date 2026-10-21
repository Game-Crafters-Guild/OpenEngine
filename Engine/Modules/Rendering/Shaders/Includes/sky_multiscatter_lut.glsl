#ifndef GE_SKY_MULTISCATTER_LUT_GLSL
#define GE_SKY_MULTISCATTER_LUT_GLSL

// The multiscatter LUT's parameterisation, shared by the pass that bakes it
// (sky_multiscatter_lut.comp) and the pass that reads it (sky_view_lut.comp).
//
// u is the sun's zenith cosine over [-1, 1], v the height over the atmosphere's thickness, each
// with the half-texel inset (Hillaire 2020's fromUnitToSubUvs) so the edge texels sit exactly on
// the ground and the top of the atmosphere. Without the inset the lowest row stands 1.6 km up and
// every near-ground reader gets the multiple scattering of thinner air.

#include "sky_transmittance_lut.glsl"

// The texture coordinate that holds (sunCosZenith, heightNorm).
vec2 GE_SkyMultiscatterLutUv(float sunCosZenith, float heightNorm, vec2 lutSize)
{
    return vec2(GE_SkyLutUnitToTexCoord(clamp(sunCosZenith * 0.5 + 0.5, 0.0, 1.0), lutSize.x),
                GE_SkyLutUnitToTexCoord(clamp(heightNorm, 0.0, 1.0), lutSize.y));
}

// Inverse of GE_SkyMultiscatterLutUv: the (sunCosZenith, heightNorm) a texel centre stands for.
vec2 GE_SkyMultiscatterLutParams(vec2 uv, vec2 lutSize)
{
    return vec2(GE_SkyLutTexCoordToUnit(uv.x, lutSize.x) * 2.0 - 1.0,
                clamp(GE_SkyLutTexCoordToUnit(uv.y, lutSize.y), 0.0, 1.0));
}

#endif // GE_SKY_MULTISCATTER_LUT_GLSL
