// Shared screen-space helpers: the opaque scene-depth sampler, the reverse-Z eye depth and world
// position from a raw depth, and a screen-UV->texel clamp. Used by the StandardPBR transmission lobe
// and the ocean surface refraction, so the depth-guard math lives in one place.
//
// Requires ge_invProj, ge_invView and ge_screenSize from view_params.glsl (set 0 binding 7),
// which the forward adapter includes before any surface/IBL shader.

#ifndef GE_SCREEN_SPACE_GLSL
#define GE_SCREEN_SPACE_GLSL

// Opaque scene depth (set 0 binding 17): the resolved opaque depth the world pass
// binds by name to every pass. Single-sample sampler2D, safe at any MSAA level.
layout(set = 0, binding = 17) uniform sampler2D ge_sceneDepth;

// View-space eye distance from a reverse-Z raw depth at a screen UV. ge_invProj is
// the true projection inverse, so this is correct under reverse-Z (near->1, far->0):
// un-project to view space, perspective-divide, take |z|. Direction-agnostic, so
// callers compare reconstructed eye distances (not raw depths) and the reverse-Z
// sign never bites.
float GE_EyeDepthFromRaw(float rawDepth, vec2 screenUV)
{
    vec2 ndc = screenUV * 2.0 - 1.0;
    vec4 viewP = ge_invProj * vec4(ndc, rawDepth, 1.0);
    viewP.xyz /= max(abs(viewP.w), 1e-5);
    return abs(viewP.z);
}

// World-space position of the surface at a screen UV with a reverse-Z raw depth. The viewport's
// height is negative, so V grows downward and NDC y is 1 - 2v.
vec3 GE_WorldPositionFromRaw(float rawDepth, vec2 screenUV)
{
    vec2 ndc = vec2(screenUV.x * 2.0 - 1.0, 1.0 - 2.0 * screenUV.y);
    vec4 viewP = ge_invProj * vec4(ndc, rawDepth, 1.0);
    viewP.xyz /= max(abs(viewP.w), 1e-5);
    return (ge_invView * vec4(viewP.xyz, 1.0)).xyz;
}

ivec2 GE_ScreenTexel(vec2 screenUV)
{
    ivec2 size = max(ivec2(ge_screenSize.xy), ivec2(1));
    ivec2 maxCoord = size - ivec2(1);
    return clamp(ivec2(floor(clamp(screenUV, vec2(0.0), vec2(1.0)) * vec2(size))),
                 ivec2(0), maxCoord);
}

#endif // GE_SCREEN_SPACE_GLSL
