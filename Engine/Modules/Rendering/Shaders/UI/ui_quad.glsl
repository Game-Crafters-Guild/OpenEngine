// Quad placement shared by the UI vertex stages. Needs the push-constant
// block from ui_sdf_common.glsl.

#ifndef UI_QUAD_GLSL
#define UI_QUAD_GLSL

// Pixels are top-left origin; clip +Y is the top row on every backend, so the
// mapping negates Y once.
vec4 clipFromPixel(vec2 pixel)
{
    vec2 ndc = (pixel / pc.targetSize) * 2.0 - 1.0;
    return vec4(ndc.x, -ndc.y, 0.0, 1.0);
}

vec2 quadPos01(uint vid)
{
    // Two triangles: (0,0)-(1,0)-(1,1) and (0,0)-(1,1)-(0,1)
    if (vid == 0u) return vec2(0.0, 0.0);
    if (vid == 1u) return vec2(1.0, 0.0);
    if (vid == 2u) return vec2(1.0, 1.0);
    if (vid == 3u) return vec2(0.0, 0.0);
    if (vid == 4u) return vec2(1.0, 1.0);
    return vec2(0.0, 1.0);
}

#endif // UI_QUAD_GLSL
