#version 450
// Test-only: the volumetric clouds in front of an additive surface beyond them (forward_fog_probe_body.glsl).
#define GE_CLOUDS_IN_FRONT
#define GE_BLEND_ADDITIVE
#include "forward_fog_probe_body.glsl"
