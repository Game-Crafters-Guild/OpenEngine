// The volumetric fog's phase function. Shared like volumetric_fog_view.glsl.
#ifndef GE_VOLUMETRIC_FOG_PHASE_GLSL
#define GE_VOLUMETRIC_FOG_PHASE_GLSL

float FogHenyeyGreenstein(float cosTheta, float g)
{
    float gg = g * g;
    float denom = max(1.0 + gg - 2.0 * g * cosTheta, 1.0e-4);
    return (1.0 - gg) / (12.5663706 * denom * sqrt(denom));
}

#endif // GE_VOLUMETRIC_FOG_PHASE_GLSL
