#version 450
// Test-only: the height fog on a multiply surface (forward_fog_probe_body.glsl).
#define GE_HEIGHT_FOG
#define GE_BLEND_MULTIPLY
#include "forward_fog_probe_body.glsl"
