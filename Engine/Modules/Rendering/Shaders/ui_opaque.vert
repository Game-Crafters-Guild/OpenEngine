#version 450

// Opaque interiors of the UI draw list (UI::UIOpaqueInteriors): one quad per
// entry, at the depth of its draw-order position. The UI pass draws them front
// to back before the main draw, which then shades only what lies above them.

#include "UI/ui_sdf_common.glsl"
#include "UI/ui_quad.glsl"

struct UIOpaqueInterior {
    vec4 rect;     // x, y, w, h in target pixels
    uvec4 params;  // .x = fill color (packed RGBA8), .y = draw-order position, .zw = unused
};

layout(std430, set = 0, binding = 0) readonly buffer OpaqueBuf {
    UIOpaqueInterior interiors[];
} ob;

layout(location = 0) flat out uint vFillColor;

void main()
{
    UIOpaqueInterior interior = ob.interiors[gl_InstanceIndex];
    vec2 pixel = interior.rect.xy + quadPos01(uint(gl_VertexIndex) % 6u) * interior.rect.zw;
    vFillColor = interior.params.x;
    gl_Position = clipFromPixel(pixel);
    gl_Position.z = uiDrawOrderDepth(interior.params.y);
}
