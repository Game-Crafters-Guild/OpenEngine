#version 450
// Test-only: the volumetric fog on a premultiplied-alpha surface (forward_fog_probe_body.glsl).
#define GE_VOLUMETRIC_FOG_FORWARD
#define GE_BLEND_PREMULTIPLIED
#include "forward_fog_probe_body.glsl"
