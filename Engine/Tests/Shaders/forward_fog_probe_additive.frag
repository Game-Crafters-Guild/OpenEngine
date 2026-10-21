#version 450
// Test-only: the height fog on an additive surface (forward_fog_probe_body.glsl).
#define GE_HEIGHT_FOG
#define GE_BLEND_ADDITIVE
#include "forward_fog_probe_body.glsl"
