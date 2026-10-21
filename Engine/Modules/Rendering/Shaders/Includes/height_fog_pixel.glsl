// The height fog in front of what a screen pixel shows, from the view depth: the sky fill where the
// depth is the cleared far plane, the fog at the surface's distance elsewhere. For the passes that
// work on the resolved scene after the surfaces took their own fog (the cloud composites, which put
// a cloud in front of what a pixel shows, and the fog glow), so they agree with the term each
// surface evaluated. One depth per pixel (sample 0 of a multisampled depth).
//
// The includer declares, before this file: the ViewParams block (its core prefix) under the instance
// name ViewParams, and what height_fog_term.glsl asks for (the HeightFogParams block and
// GE_HEIGHT_FOG_BACKDROP).
#ifndef GE_HEIGHT_FOG_PIXEL_GLSL
#define GE_HEIGHT_FOG_PIXEL_GLSL

#include "height_fog_term.glsl"

/// The height fog in front of the pixel at uv in [0, 1] whose view depth is rawDepth (reverse-Z, 0 is
/// the cleared far plane). scattering is absolute scene-linear; the fogged radiance is
/// radiance * transmittance + scattering.
void GE_HeightFogAtPixel(vec2 uv, float rawDepth, out vec3 scattering, out float transmittance)
{
    // Reverse-Z clears to exactly 0, and a D32F buffer keeps the farthest geometry above it, so
    // only 0 is sky.
    bool sky = rawDepth <= 0.0;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    vec4 pV = ViewParams.ge_invProj * vec4(ndc, rawDepth, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    vec3 viewDirWS = normalize(transpose(mat3(ViewParams.ge_view)) * normalize(pV.xyz));
    vec3 originWS = ViewParams.ge_cameraPosWS.xyz;
    if (sky)
        GE_HeightFogSky(originWS, viewDirWS, ViewParams.ge_nearFar.y, uv, scattering, transmittance);
    else
        GE_HeightFogSurface(originWS, viewDirWS, length(pV.xyz), uv, scattering, transmittance);
}

#endif // GE_HEIGHT_FOG_PIXEL_GLSL
