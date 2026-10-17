#version 450

// Test-only probe for FogForwardParityTests: what a transparent fragment drawn after the
// VolumetricFog composite makes of itself through the fog. It is the forward path as it ships,
// GE_ApplyVolumetricFog (forward_volumetric_fog.glsl), with the fog's parameters, its volume, the
// irradiance cube and the view bound where a forward material binds them, for a fragment of colour
// pc.color at pc.positionWS.xyz with scatter weight pc.positionWS.w. The test draws
// volumetric_fog_composite.frag over an opaque pixel at the same place and asserts the two agree.

#include "Includes/forward_volumetric_fog.glsl"

layout(location = 0) out vec4 oColor;

layout(push_constant) uniform ProbePC
{
    vec4 color;
    vec4 positionWS; // w: scatter weight
} pc;

void main()
{
    oColor = vec4(GE_ApplyVolumetricFog(pc.color.rgb, pc.positionWS.xyz, pc.positionWS.w), 1.0);
}
