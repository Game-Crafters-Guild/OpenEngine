#ifndef GE_SUN_GLARE_VS_GLSL
#define GE_SUN_GLARE_VS_GLSL

// Shared vertex stage for the sun glare pass. Both depth-source variants -- sun_glare.vert
// (single-sample scene depth) and sun_glare_ms.vert (multisampled) -- include this after
// declaring their own depth uniform and defining GE_SUN_GLARE_PROBE(sunUV, radiusUV) to call
// the matching visibility function. Nothing else differs between them, so nothing else is
// duplicated: the shaderpkg packager has no keyword mechanism, so a variant is a second file,
// and a second file is only safe when it is this thin.
//
// Everything the glare needs that does NOT vary per pixel is evaluated here and handed on as
// flat varyings: the transmittance along the ray to the sun, and whether the direct beam
// reaches the camera at all. All of it is a function of push constants and ONE world position,
// so evaluating it per fragment would repeat the same fetches for every pixel of a fullscreen
// draw. Three vertices, one result. The cascade term is the exception that is not evaluated
// here: its 700-odd lookups run in parallel in a pass of their own (sun_glare_casters.comp),
// and this stage reads the one float that pass writes.

#include "Includes/sun_glare.glsl"
#include "Includes/sky_transmittance_lut.glsl"

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out vec3 vSunTransmittance;
layout(location = 2) flat out float vSunGlareScale;

layout(set = 0, binding = 0) uniform sampler2D uTransLUT;

// The cascade term sun_glare_casters.comp wrote this frame: the fraction of the sun's disc whose
// beam reaches the eye past every shadow caster. Read only when pc.horizon.w says it is real.
layout(set = 0, binding = 3, std430) readonly buffer SunGlareCasters
{
    float casters;
} uCasters;

layout(push_constant) uniform GlarePC
{
    // xyz = world-space direction TOWARD the sun (unit), w = sun angular radius (rad).
    vec4 sunDirRadius;
    // xyz = sun colour (scene-linear), w = above-atmosphere irradiance E.
    vec4 sunColorIrradiance;
    // xy = sun's projected position in viewport UV, zw = probe ring radius in UV, given PER
    // AXIS so the ring is a circle in pixels rather than stretched by the viewport aspect.
    vec4 probe;
    // x = the sky's own scaling of the drawn sun: its exposure trim times the day-to-night
    // fade, so the halo tracks the disc. y = cosine of the sun's zenith angle at the camera,
    // z = normalised camera altitude in the atmosphere (both are the transmittance LUT's
    // coordinates). w = the scene depth texture's SAMPLE COUNT when the screen probe is
    // meaningful (depth bound, sun on screen), and 0 when it is not -- one field carrying both
    // because the block's 80-byte size is asserted against the shaders on both sides.
    vec4 params;
    // x = terrain-skyline visibility in [0, 1], marched on the CPU against the heightfield so
    // it holds whether or not the sun is on screen. y = planet radius, z = atmosphere radius
    // (the transmittance LUT's shell, meters). w = 1 when uCasters holds this frame's cascade
    // term, 0 when the view has no cascades.
    vec4 horizon;
} pc;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);

    // The transmittance LUT read along the SUN's direction, which is the ray the sun's light
    // actually travels down — the disc's own lookup keyed on the view direction reduces to this
    // one at the disc's centre. 0 once the sun is below the camera's horizon.
    float planetRadius = pc.horizon.y;
    float atmosphereRadius = pc.horizon.z;
    float camR = mix(planetRadius, atmosphereRadius, clamp(pc.params.z, 0.0, 1.0));
    vSunTransmittance = GE_SkyTransmittanceToTop(uTransLUT, camR, clamp(pc.params.y, -1.0, 1.0),
                                                 planetRadius, atmosphereRadius);

    // Shadow cascades: anything that CASTS, on screen or off, including a sun behind the
    // camera, as the fraction of the disc the beam reaches.
    float sunlit = pc.horizon.w > 0.5 ? clamp(uCasters.casters, 0.0, 1.0) : 1.0;

    // Scene depth: anything in the depth attachment the world pass wrote, CBT terrain
    // included. Only meaningful while the sun is on screen, so it is gated rather than
    // guessed — params.w is the sample count then and 0 otherwise.
    float onScreen = pc.params.w >= 1.0 ? GE_SUN_GLARE_PROBE(pc.probe.xy, pc.probe.zw) : 1.0;

    // One flat scalar rather than four: the sky's own scaling of the drawn sun and all three
    // visibility terms are per-frame constants, so the fragment stage has no reason to
    // see them separately — and keeping them out of its push-constant block is what lets that
    // block stay a 32-byte prefix of this one (the packager sums the per-stage ranges against
    // a 128-byte limit).
    // The cascades see casting meshes anywhere and the skyline sees terrain anywhere: disjoint
    // occluders, so their fractions multiply. The screen probe sees both while the sun is on
    // screen, so it is combined by the minimum rather than multiplied in, which would count an
    // occluder both of them see twice (sun_glare.glsl, "Visibility").
    vSunGlareScale = pc.params.x * min(onScreen, sunlit * pc.horizon.x);
}

#endif // GE_SUN_GLARE_VS_GLSL
