#version 450
// Test-only: the height fog in front of what a pixel shows, from its depth, as the passes that work
// on the resolved scene evaluate it (height_fog_pixel.glsl: the cloud composites and the fog glow),
// over the scene colour: scene * T + S * P.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;
layout(set = 0, binding = 3, std140) uniform HeightFogParamsBlock
{
#include "Includes/height_fog_params_fields.glsl"
} HeightFogParams;
layout(set = 0, binding = 4) uniform sampler2D uSkyBackdrop;
#define GE_EXPOSURE_HISTORY_BINDING 5
#include "Includes/exposure_history.glsl"
#define GE_HEIGHT_FOG_BACKDROP uSkyBackdrop
#include "Includes/height_fog_pixel.glsl"

void main()
{
    vec4 scene = texture(uSceneColor, vUV);
    ivec2 depthSize = textureSize(uDepth, 0);
    ivec2 pixel = ivec2(clamp(vUV * vec2(depthSize), vec2(0.0), vec2(depthSize) - vec2(1.0)));
    vec3 scattering;
    float transmittance;
    GE_HeightFogAtPixel(vUV, texelFetch(uDepth, pixel, 0).r, scattering, transmittance);
    oColor = vec4(scene.rgb * transmittance + scattering * GE_PreExposure(), scene.a);
}
