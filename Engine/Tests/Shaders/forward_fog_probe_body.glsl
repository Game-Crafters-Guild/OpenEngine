// Test-only: the fog a forward shading variant applies to the surface it draws, as
// adapter_forward.glsl applies it (forward_fog.glsl, with the view and the exposure state the adapter
// binds), for one surface of colour pc.color.rgb and alpha pc.color.a at pc.positionWS.xyz, written
// as the adapter writes it: stored at the pre-exposure, with the surface's alpha. The including
// probe defines the fog keywords (GE_HEIGHT_FOG, GE_VOLUMETRIC_FOG_FORWARD, GE_CLOUDS_IN_FRONT) and the blend
// keyword.
#include "Includes/view_exposure.glsl"
#include "Includes/forward_fog.glsl"

layout(location = 0) out vec4 oColor;

layout(push_constant) uniform ProbePC
{
    vec4 color;
    vec4 positionWS;
} pc;

void main()
{
    vec2 uv = gl_FragCoord.xy * ge_screenSize.zw;
    GE_ForwardFog fog = GE_EvaluateForwardFog(pc.positionWS.xyz, uv);
    vec3 color = GE_ApplyForwardFog(pc.color.rgb, pc.color.a, vec3(1.0), fog);
    float alpha = pc.color.a;
#ifdef GE_CLOUDS_IN_FRONT
    GE_ApplyCloudsInFront(pc.positionWS.xyz, uv, color, alpha);
#endif
    oColor = vec4(color * GE_PreExposure(), alpha);
}
