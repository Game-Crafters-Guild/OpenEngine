#version 450
// Test-only: the volumetric fog on an additive surface (forward_fog_probe_body.glsl).
#define GE_VOLUMETRIC_FOG_FORWARD
#define GE_BLEND_ADDITIVE
#include "forward_fog_probe_body.glsl"
