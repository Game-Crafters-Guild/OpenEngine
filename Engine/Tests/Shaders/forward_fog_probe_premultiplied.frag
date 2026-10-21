#version 450
// Test-only: the height fog on a premultiplied-alpha surface (forward_fog_probe_body.glsl).
#define GE_HEIGHT_FOG
#define GE_BLEND_PREMULTIPLIED
#include "forward_fog_probe_body.glsl"
