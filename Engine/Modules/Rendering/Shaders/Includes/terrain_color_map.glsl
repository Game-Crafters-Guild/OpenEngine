// The terrain colour map's apply, shared by the ground (cbt_surface.glsl) and the blade root
// (grass_root_ground_color.glsl on the full profile, terrain_grass_surface.glsl on the compat one), so
// a blade's base and the ground beside it take the same colour under a stroke and under the grass's
// own written soil.
//
// A colour-map texel is the ground's final colour times its weight (rgb, premultiplied in linear
// light; the RGBA8_SRGB image decodes it) and the weight (a). The ground becomes
// ground x (1 - a) + rgb: one mix. vec4(0) is identity, exactly.
//
// The literals carry the `f` suffix and the block reads components rather than swizzles because it
// is lifted verbatim into the host tests (GlslShim.h).

#ifndef GE_TERRAIN_COLOR_MAP_GLSL
#define GE_TERRAIN_COLOR_MAP_GLSL

// GE_SHARED_TERRAIN_COLOR_MAP_BEGIN
vec3 TerrainApplyColorMap(vec3 ground, vec4 colorMap)
{
    float keep = 1.0f - colorMap.w;
    return vec3(ground.x * keep + colorMap.x, ground.y * keep + colorMap.y, ground.z * keep + colorMap.z);
}
// GE_SHARED_TERRAIN_COLOR_MAP_END

#endif // GE_TERRAIN_COLOR_MAP_GLSL
