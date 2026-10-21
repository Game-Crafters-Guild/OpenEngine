// The fog in front of a surface that a forward shading variant draws, evaluated in the surface's
// own shader by its own distance: the volumetric fog (VolumetricFog pass keyword,
// volumetric_fog_forward.glsl) and the height fog over it (HeightFog pass keyword,
// height_fog_term.glsl), the order their effects compose in. A fragment shaded once per pixel under
// MSAA writes its colour to the samples its own primitive covers, so the fog it carries lands on
// exactly those samples: an anti-aliased edge keeps each side's own fog.
//
// The surface's blend decides how the fog joins its colour: the fog fades it toward the colour
// that leaves what is behind it unchanged and adds the fog's light only as far as the surface hides
// what is behind it (the blend keywords, MaterialKeyword::PremultipliedBlend and its siblings).
//
// The includer includes view_params.glsl and view_exposure.glsl first. Units: positions world space
// (left-handed, +Y up), distances meters, radiance absolute scene-linear (the caller's colour is
// absolute until it is stored at the pre-exposure).
#ifndef GE_FORWARD_FOG_GLSL
#define GE_FORWARD_FOG_GLSL

#ifdef GE_HEIGHT_FOG
layout(set = 0, binding = 52, std140) uniform HeightFogParamsBlock
{
#include "height_fog_params_fields.glsl"
} HeightFogParams;
// What the sky pass draws its backdrop from (the sky-view LUT or the HDRI); read only when the
// fog's colour comes from the sky.
layout(set = 0, binding = 53) uniform sampler2D ge_skyBackdrop;
#define GE_HEIGHT_FOG_BACKDROP ge_skyBackdrop
#include "height_fog_term.glsl"
#endif

#ifdef GE_VOLUMETRIC_FOG_FORWARD
#include "volumetric_fog_forward.glsl"
#endif

#ifdef GE_CLOUDS_IN_FRONT
// The view's resolved volumetric clouds (R light energy, G transmittance, B the transmittance-weighted
// mean cloud distance in meters), composited over the scene before the blended surfaces that read it
// are drawn.
layout(set = 0, binding = 57) uniform sampler2D ge_volumetricClouds;

/// A blended surface beyond the clouds' mean distance at its pixel stands behind the clouds, which
/// were composited over the scene before it was drawn: its contribution takes the clouds'
/// transmittance, as its blend composites it. A nearer surface is drawn over the clouds unchanged.
void GE_ApplyCloudsInFront(vec3 positionWS, vec2 uv, inout vec3 color, inout float alpha)
{
    vec4 clouds = textureLod(ge_volumetricClouds, uv, 0.0);
    if (length(positionWS - ge_cameraPosWS.xyz) <= clouds.b)
        return;
    float cloudTransmittance = clamp(clouds.g, 0.0, 1.0);
  #if defined(GE_BLEND_UNFOGGED)
    return;
  #elif defined(GE_BLEND_MULTIPLY)
    color = mix(vec3(1.0 / GE_PreExposure()), color, cloudTransmittance);
  #elif defined(GE_BLEND_ADDITIVE)
    color *= cloudTransmittance;
  #elif defined(GE_BLEND_PREMULTIPLIED)
    color *= cloudTransmittance;
    alpha *= cloudTransmittance;
  #else
    alpha *= cloudTransmittance;
  #endif
}
#endif

/// The fog between the camera and a surface: the in-scattered radiance S (absolute scene-linear)
/// and the transmittance T, so the fogged radiance of an opaque surface is radiance * T + S.
struct GE_ForwardFog
{
    vec3 Scattering;
    float Transmittance;
};

/// The fog in front of the surface at positionWS, seen at the screen position uv in [0, 1].
GE_ForwardFog GE_EvaluateForwardFog(vec3 positionWS, vec2 uv)
{
    GE_ForwardFog fog;
    fog.Scattering = vec3(0.0);
    fog.Transmittance = 1.0;
#ifdef GE_VOLUMETRIC_FOG_FORWARD
    vec4 volumetric = GE_VolumetricFogForward(uv, positionWS);
    fog.Scattering = volumetric.rgb;
    fog.Transmittance = volumetric.a;
#endif
#ifdef GE_HEIGHT_FOG
    vec3 toSurface = positionWS - ge_cameraPosWS.xyz;
    float surfaceDistance = length(toSurface);
    if (surfaceDistance > 0.0)
    {
        vec3 heightScattering;
        float heightTransmittance;
        GE_HeightFogSurface(ge_cameraPosWS.xyz, toSurface / surfaceDistance, surfaceDistance, uv,
                            heightScattering, heightTransmittance);
        fog.Scattering = fog.Scattering * heightTransmittance + heightScattering;
        fog.Transmittance *= heightTransmittance;
    }
#endif
    return fog;
}

/// The surface's colour seen through `fog`, as its blend composites it.
/// @param color          the surface's radiance, absolute scene-linear (a multiply tint for a
///                       multiply blend).
/// @param alpha          the surface's output alpha.
/// @param scatterWeight  how much of the fog's light the surface takes per channel, in [0, 1]: 1
///                       for a surface that hides what is behind it; glass passes 1 minus what it
///                       lets through of a background that already carries the fog.
vec3 GE_ApplyForwardFog(vec3 color, float alpha, vec3 scatterWeight, GE_ForwardFog fog)
{
#if defined(GE_BLEND_UNFOGGED)
    return color;
#elif defined(GE_BLEND_MULTIPLY)
    // Toward the tint that leaves what is behind it unchanged: 1 once stored at the pre-exposure.
    return mix(vec3(1.0 / GE_PreExposure()), color, fog.Transmittance);
#elif defined(GE_BLEND_ADDITIVE)
    return color * fog.Transmittance;
#elif defined(GE_BLEND_PREMULTIPLIED)
    return color * fog.Transmittance + fog.Scattering * scatterWeight * alpha;
#else
    return color * fog.Transmittance + fog.Scattering * scatterWeight;
#endif
}

#endif // GE_FORWARD_FOG_GLSL
