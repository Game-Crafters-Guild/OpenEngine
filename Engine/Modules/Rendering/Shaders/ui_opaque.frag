#version 450

// The fill of an opaque UI interior. It is the value ui_sdf.frag emits for a
// plain rect fill at full coverage, through the same paint helpers, so the
// pre-pass and the main draw give the same bytes.

#include "UI/ui_sdf_common.glsl"

layout(location = 0) flat in uint vFillColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = toPremultipliedSdrUi(uiPaintColor(vFillColor));
}
