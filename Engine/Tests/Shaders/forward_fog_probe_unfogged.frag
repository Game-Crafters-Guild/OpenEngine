#version 450
// Test-only: the height fog on a surface whose blend has no identity colour (forward_fog_probe_body.glsl).
#define GE_HEIGHT_FOG
#define GE_BLEND_UNFOGGED
#include "forward_fog_probe_body.glsl"
