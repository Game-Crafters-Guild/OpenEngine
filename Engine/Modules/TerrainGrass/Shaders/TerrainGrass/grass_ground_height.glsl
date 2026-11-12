// The terrain maps and pages the grass kernels read the ground through, and the ground height they
// read. Set 0 is the one layout the classify, plan and place kernels share (MakeGrassPlaceDescriptorSetLayout).
// The includer declares TerrainParamsEntry first.

#ifndef GRASS_GROUND_HEIGHT_GLSL
#define GRASS_GROUND_HEIGHT_GLSL


// The compat arm reads every map through the named bindings below, so it does not declare the set 1
// texture slots. The compute layout has no set 1, and Metal rejects a pipeline that names it.
#if !defined(GE_COMPAT_PROFILE)
#include "Includes/bindless_textures.glsl"
#endif

// The terrain lattice helper (CBT_LatticeTextureUV) the unified map taps read through. Spelled
// "cbt_atlas.glsl": this stage's compile has the CBT directory on its include path.
#include "cbt_atlas.glsl"

// Unified-map existence bits (Terrain/TerrainTypes.h mirror): on the compat profile the bindless
// indices are always 0, so map existence rides these flags and the taps read the NAMED bindings.
#if defined(GE_COMPAT_PROFILE)
const uint kTerrainFlagHasHeightmap = 4u;
const uint kTerrainFlagHasNormalmap = 8u;
const uint kTerrainFlagHasSplatmap = 16u;
#define GRASS_MISSING_HEIGHTMAP(tp) ((tp.Flags & kTerrainFlagHasHeightmap) == 0u)
#define GRASS_HAS_NORMALMAP(tp) ((tp.Flags & kTerrainFlagHasNormalmap) != 0u)
#define GRASS_HAS_SPLATMAP(tp) ((tp.Flags & kTerrainFlagHasSplatmap) != 0u)
#else
#define GRASS_MISSING_HEIGHTMAP(tp) (tp.HeightmapBindless == 0u)
#define GRASS_HAS_NORMALMAP(tp) (tp.NormalmapBindless != 0u)
#define GRASS_HAS_SPLATMAP(tp) (tp.SplatmapBindless != 0u)
#endif

// grassMapTap returns a terrain map's texel at a UV in [0, 1], mip 0; grassMapSize returns its size in texels.
// The full profile reads the bindless index, compat reads the named texture; each arm expands only its
// own operand. In both arms, index 0 means no map is bound.
#if defined(GE_COMPAT_PROFILE)
#define grassMapTap(mapIndex, mapTex, uv) textureLod(sampler2D(mapTex, gGrassMapSampler), uv, 0.0)
#define grassMapSize(mapIndex, mapTex) textureSize(sampler2D(mapTex, gGrassMapSampler), 0)
#else
#define grassMapTap(mapIndex, mapTex, uv) textureLod(GE_BTEX(mapIndex, GE_TS_CLAMP), uv, 0.0)
#define grassMapSize(mapIndex, mapTex) textureSize(GE_BTEX(mapIndex, GE_TS_CLAMP), 0)
#endif
// A unified height, normal, splat or grass control map read at a terrain UV: those maps are
// terrain lattices (CBT_LatticeTextureUV), so the blade stands on the surface the terrain draws.
#define grassLatticeTap(mapIndex, mapTex, uv) \
    grassMapTap(mapIndex, mapTex, CBT_LatticeTextureUV(uv, vec2(grassMapSize(mapIndex, mapTex))))

#if defined(GE_COMPAT_PROFILE)
// WebGPU exposes no binding arrays, so the four unified maps this shader reads are named bindings in
// grass's own set. One sampler serves all of them: every tap here is a clamped explicit-LOD data
// lookup (GE_TS_CLAMP on the bindless arm).
layout(set = 0, binding = 13) uniform texture2D gGrassHeightmap;
layout(set = 0, binding = 14) uniform texture2D gGrassNormalmap;
layout(set = 0, binding = 15) uniform texture2D gGrassSplatmap;
layout(set = 0, binding = 18) uniform texture2D gGrassControlmap;
layout(set = 0, binding = 19) uniform sampler gGrassMapSampler;
#define GRASS_SPLAT_TAP_UNIFIED(bindlessIndex, uv) grassLatticeTap(bindlessIndex, gGrassSplatmap, uv)
#else
// The paged terrain's page-table words (the height table, then the surface table: the CBT's binding-22
// ring slot), its height cache and its surface plane caches, for cbt_surface_pages.glsl.
layout(std430, set = 0, binding = 5) readonly buffer GrassPageTableBuffer
{
    uint Words[];
} gPageTable;
layout(set = 0, binding = 21) uniform sampler2D gPageCache;
layout(set = 0, binding = 22) uniform sampler2D cbt_SurfaceSplat;
layout(set = 0, binding = 23) uniform sampler2D cbt_SurfaceNormal;
layout(set = 0, binding = 24) uniform sampler2D cbt_SurfaceGrass;
layout(set = 0, binding = 25) uniform sampler2D cbt_SurfaceColor;
#define GRASS_SURFACE_SET 0
#define GRASS_SURFACE_PARAMS_BINDING 4
#endif
// The surface page resolve (its struct only on the compat profile, which has no pages).
#include "cbt_surface_pages.glsl"

// The ground resolve, shared with terrain_grass_surface.glsl so a blade's placement mask and its
// ground color read one answer.
#include "TerrainGrass/grass_surface_pages.glsl"

float sampleHeight01(TerrainParamsEntry tp, vec2 uv)
{
    if (GRASS_MISSING_HEIGHTMAP(tp))
        return 0.0;
    return grassLatticeTap(tp.HeightmapBindless, gGrassHeightmap, uv).r;
}

// The ground at `uv` in [0, 1] of the height range of terrains[row]: its pages where its height is
// paged, else its unified heightmap.
float GrassGroundHeight01(uint row, TerrainParamsEntry tp, vec2 uv)
{
#if defined(GE_COMPAT_PROFILE)
    return sampleHeight01(tp, uv);
#else
    return GrassSurface_HeightPaged(row) ? GrassSurface_PagedHeight01(uv) : sampleHeight01(tp, uv);
#endif
}

#endif // GRASS_GROUND_HEIGHT_GLSL
