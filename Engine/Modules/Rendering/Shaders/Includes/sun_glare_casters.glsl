#ifndef GE_SUN_GLARE_CASTERS_GLSL
#define GE_SUN_GLARE_CASTERS_GLSL

// The sun glare's cascade term as one workgroup: the fraction of the sun's disc whose beam
// reaches the eye past everything the shadow cascades store. Shared by the pass that computes
// it (sun_glare_casters.comp) and the probe that tests it (sun_glare_probe.comp), so the value
// a frame uses and the value the test measures cannot drift apart.
//
// One lane per tap and cascade, GE_SUN_GLARE_CASTERS_LANES in all. A tap's lookups depend on
// nothing but its own position, so the whole term costs the latency of one tap rather than of
// sixteen taps in a row per cascade.
//
// The includer declares local_size_x = GE_SUN_GLARE_CASTERS_LANES and calls
// GE_SunGlareCastersVisibility from every lane, in uniform control flow (it holds a barrier).

#include "Includes/sun_glare.glsl"

#define GE_SUN_GLARE_CASTERS_CASCADES 4
#define GE_SUN_GLARE_CASTERS_LANES (GE_SUN_GLARE_PROBE_TAPS * GE_SUN_GLARE_CASTERS_CASCADES)

shared float gSunGlareTapVisible[GE_SUN_GLARE_CASTERS_LANES];
shared float gSunGlareTapWeight[GE_SUN_GLARE_CASTERS_LANES];

// The cascade this lane evaluates. The caller hands GE_SunGlareCastersVisibility that cascade's
// light view-projection, read straight from its own block: an array passed by value and indexed
// by lane would be copied to scratch memory.
int GE_SunGlareCastersLaneCascade()
{
    return int(gl_LocalInvocationIndex) / GE_SUN_GLARE_PROBE_TAPS;
}

// `cascadeCount` cascades are read, at most four; `laneCascadeVP` is the light view-projection
// of GE_SunGlareCastersLaneCascade(), any matrix on a lane past the count. The result is the
// MINIMUM over cascades, not the cascade a view depth would select: each cascade's map contains
// only the casters inside its own light frustum, and the beam has to survive the whole way; a
// cascade that does not cover the eye reads clear and drops out of the minimum harmlessly.
// Every lane returns the same value.
float GE_SunGlareCastersVisibility(sampler2DArrayShadow shadowArray, mat4 laneCascadeVP,
                                   int cascadeCount, vec3 eyeWS, vec3 sunDirWS, float discRadius,
                                   float depthBias)
{
    int lane = int(gl_LocalInvocationIndex);
    int tapIndex = lane % GE_SUN_GLARE_PROBE_TAPS;
    int cascade = GE_SunGlareCastersLaneCascade();

    vec2 tap;
    float weight;
    GE_SunGlareDiscTap(tapIndex, tap, weight);
    float beam = 1.0;
    if (cascade < cascadeCount)
    {
        GE_SunGlareCascadeFrame frame = GE_SunGlareCascadeFrameAt(
            shadowArray, laneCascadeVP, cascade, eyeWS, sunDirWS, discRadius, depthBias);
        beam = GE_SunGlareCascadeTapBeam(shadowArray, cascade, frame, tap, depthBias);
    }
    gSunGlareTapVisible[lane] = beam * weight;
    gSunGlareTapWeight[lane] = weight;
    barrier();

    float visibility = 1.0;
    for (int c = 0; c < GE_SUN_GLARE_CASTERS_CASCADES; ++c)
    {
        if (c >= cascadeCount)
            break;
        float visible = 0.0;
        float total = 0.0;
        for (int t = 0; t < GE_SUN_GLARE_PROBE_TAPS; ++t)
        {
            visible += gSunGlareTapVisible[c * GE_SUN_GLARE_PROBE_TAPS + t];
            total += gSunGlareTapWeight[c * GE_SUN_GLARE_PROBE_TAPS + t];
        }
        visibility = min(visibility, visible / max(total, 1e-6));
    }
    return visibility;
}

#endif // GE_SUN_GLARE_CASTERS_GLSL
