// The fog's own light at a pixel, for the fog glow: the height fog in front of what the pixel shows
// (height_fog_pixel.glsl), the term the surfaces evaluated in their own shaders. The glow is a blur,
// so one depth per pixel is enough.
//
// The includer declares, before this file: uDepth (the view depth), the ViewParams block (its core
// prefix), the HeightFogParams block, uSkyBackdrop, and the view's exposure state
// (exposure_history.glsl).
#ifndef GE_FOG_GLOW_LAYER_GLSL
#define GE_FOG_GLOW_LAYER_GLSL

#define GE_HEIGHT_FOG_BACKDROP uSkyBackdrop
#include "height_fog_pixel.glsl"

/// rgb: the fog's in-scattered light at the pixel, stored at the frame's pre-exposure like scene
/// colour; a: the fog's opacity there, 1 - transmittance.
vec4 GE_FogGlowLayer(vec2 uv)
{
    ivec2 depthSize = textureSize(uDepth, 0);
    ivec2 pixel = ivec2(clamp(uv * vec2(depthSize), vec2(0.0), vec2(depthSize) - vec2(1.0)));
    vec3 scattering;
    float transmittance;
    GE_HeightFogAtPixel(uv, texelFetch(uDepth, pixel, 0).r, scattering, transmittance);
    return vec4(scattering * GE_PreExposure(), 1.0 - transmittance);
}

#endif // GE_FOG_GLOW_LAYER_GLSL
