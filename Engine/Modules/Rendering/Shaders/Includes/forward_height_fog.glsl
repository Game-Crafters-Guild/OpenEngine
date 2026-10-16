// Height fog for what a forward pass draws over the already fogged scene (GE_HEIGHT_FOG): a
// fragment fogged by its own distance from the camera, with the sum the HeightFog post-process
// fogs the opaque scene by its depth (height_fog_common.glsl), so the two agree where they meet.
//
// The post-process cannot fog a transparent fragment: it reads only the opaque depth, so it
// would fog the fragment as far away as what lies behind it, and not at all over the sky.
// Transparent draws that fog themselves therefore draw after it (the PostFogTransparent node).
//
// The fog's parameters are the HeightFogParams buffer the HeightFogParamsUpload node publishes
// (bound by name; a view without one binds a fallback that fogs nothing), and the sky colour
// sources read the SkyRender node's backdrop.
#ifndef GE_FORWARD_HEIGHT_FOG_GLSL
#define GE_FORWARD_HEIGHT_FOG_GLSL

#include "view_params.glsl"

layout(set = 0, binding = 52, std140) uniform HeightFogParamsBlock
{
#include "height_fog_params_fields.glsl"
} HeightFogParams;

layout(set = 0, binding = 53) uniform sampler2D ge_skyBackdrop;

#define GE_HEIGHT_FOG_SKY_BACKDROP ge_skyBackdrop
#include "height_fog_common.glsl"

// color seen through the fog between the camera and positionWS: dimmed to the fog's
// transmittance, with the light the fog scatters in added as far as the fragment hides what is
// behind it, by scatterWeight. A straight-alpha draw takes 1 (its blend scales the colour, the
// fog's light with it, by alpha); a premultiplied one its alpha; an additive one 0, since it hides
// nothing and the fog's light is already in what is behind it.
vec3 GE_ApplyHeightFog(vec3 color, vec3 positionWS, float scatterWeight)
{
    float fogStrength = clamp(HeightFogParams.intensity, 0.0, 1.0);
    if (fogStrength <= 1e-5 || HeightFogParams.density <= 1e-5)
        return color;

    vec3 originWS = ge_cameraPosWS.xyz;
    vec3 toFragment = positionWS - originWS;
    float viewLength = length(toFragment);
    if (viewLength <= 1e-6)
        return color;

    vec3 scattering;
    float transmittance;
    ComputeHeightFog(originWS, toFragment / viewLength, viewLength, gl_FragCoord.xy * ge_screenSize.zw, scattering,
                     transmittance);
    vec3 fogged = color * clamp(transmittance, 0.0, 1.0) + scattering * scatterWeight;
    return mix(color, fogged, fogStrength);
}

#endif // GE_FORWARD_HEIGHT_FOG_GLSL
