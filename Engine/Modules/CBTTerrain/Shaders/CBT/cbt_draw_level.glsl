// cbt_draw_level.glsl: the terrain draw's level rule as both graphics stages read it (mirror CBTLayout.h
// CBTDrawLevelParams), offset-bound to this frame's ring slot on set 2. The vertex modifier moves a far
// vertex to the height chain level of its distance; the surface samples the normal chain at the level of
// the fragment's distance. Not declared under the compat profile (its kernels build no chain).
#ifndef CBT_DRAW_LEVEL_GLSL
#define CBT_DRAW_LEVEL_GLSL

struct CBTDrawLevelParamsData
{
    vec4 eye;     // xyz = the CBT camera (world), w = the level scale; 0 = keep the cached corners
    vec4 height;  // x = height scale, y = origin Y
    uvec4 lattice; // x = the level-0 samples (x | z << 16) the chain must have been built from;
                   // y = the normal chain's mips, 0 = the surface keeps the level-0 normal map
};
layout(std140, set = 2, binding = 14) uniform CBTDrawLevelBuffer
{
    CBTDrawLevelParamsData Params;
} gCbtDrawLevel;

#endif // CBT_DRAW_LEVEL_GLSL
