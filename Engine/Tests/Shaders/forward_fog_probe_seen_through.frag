#version 450
// Test-only: an opaque surface seen through a nearer surface that takes the height fog by its own
// distance afterwards, as the ocean shows its seabed (ocean_surface.glsl). pc.color.rgb is the scene
// colour the far surface at pc.behindWS wrote, stored at the pre-exposure; its fog comes off
// (GE_RemoveForwardFog) and the fog of the near surface at pc.positionWS goes on, written as the
// forward adapter writes an opaque surface.
#define GE_HEIGHT_FOG
#include "Includes/view_exposure.glsl"
#include "Includes/forward_fog.glsl"

layout(location = 0) out vec4 oColor;

layout(push_constant) uniform ProbePC
{
    vec4 color;
    vec4 positionWS;
    vec4 behindWS;
} pc;

void main()
{
    vec2 uv = gl_FragCoord.xy * ge_screenSize.zw;
    vec3 behind = GE_RemoveForwardFog(pc.color.rgb / GE_PreExposure(), pc.behindWS.xyz, uv);
    vec3 color = GE_ApplyForwardFog(behind, 1.0, vec3(1.0), GE_EvaluateForwardFog(pc.positionWS.xyz, uv));
    oColor = vec4(color * GE_PreExposure(), 1.0);
}
