// The ground colour a blade grows out of: ONE colour per blade, resolved at its root.
//
// Included by terrain_grass_vertex_modifier.glsl on the full profile. Every vertex of a blade
// resolves the same root, and the result reaches the surface through a FLAT varying, so the blade
// carries one ground colour from its root to its tip. The surface resolved the ground at each
// fragment's own world position until #4206: a leaning blade's fragments stand over different
// ground, so a textured ground drew its pattern up the blade as streaks.
//
// The resolve is the ground's own (Includes/terrain_material_albedo.glsl, the file cbt_surface.glsl
// shades the ground with), on the same splat (grass_atlas_splat.glsl, the file the placement mask
// reads), so the blade still takes the ground's colour and not an approximation of it. What
// differs is the FOOTPRINT: the taps read the mip whose texel covers kGrassRootGroundFootprint of
// ground, so the root colour is the ground's local average around the blade, not the one texel
// under it. A blade a few centimetres wide that took the texel under its root would turn every
// fine detail of the ground into blade-to-blade noise.
//
// When the terrain's macro colour map lands (#2706), it replaces this footprint average: one tap of
// that map at the root is the blade's ground colour.
//
// The includer supplies, before this file: TerrainParamsEntry, the bindless texture table
// (Includes/bindless_textures.glsl), CBT/cbt_atlas.glsl, and grass_atlas_splat.glsl with its set
// and bindings.

#ifndef GRASS_ROOT_GROUND_COLOR_GLSL
#define GRASS_ROOT_GROUND_COLOR_GLSL

// A grass draw carries blades from several terrains, so a material's bindless index varies within
// the draw: nonuniform access (GE_BTEX), as on the surface.
#define CBT_LAYER_TEX(texIdx) GE_BTEX(texIdx, GE_TS_REPEAT)
#define CBT_LAYER_TEX_EDGE(texIdx) GE_BTEX(texIdx, GE_TS_CLAMP_ANISO)
// A vertex stage has no implicit derivatives. The taps take explicit gradients, which
// GrassRootGroundColor sets to the root footprint, so every layer reads the mip whose texel spans
// that much ground at its own tiling. The per-view TAAU mip bias is a screen-space correction and
// does not apply to a footprint chosen in world metres, so the seam drops it.
#define CBT_LAYER_TAP(slot, tex, uv, ddx, ddy, bias) textureGrad(CBT_LAYER_TEX(tex), uv, ddx, ddy).rgb
#define CBT_LAYER_TAP_EDGE(slot, tex, uv, ddx, ddy, bias) \
    textureGrad(CBT_LAYER_TEX_EDGE(tex), uv, ddx, ddy).rgb
#include "Includes/terrain_material_albedo.glsl"

// The terrain's material table, the same buffer and binding the surface used to read. Nameless on
// purpose: MaterialBinder keys set-2 buffers on the reflected instance name.
layout(std430, set = 2, binding = 4) readonly buffer TerrainMaterialTableBuffer
{
    TerrainMaterialRecordData TerrainMaterials[];
};

#include "Includes/terrain_blend_width.glsl"
#include "Includes/terrain_blend_resolve.glsl"

// World metres of ground one root colour stands for. About the spacing of tufts (the shipped clump
// is 1.1 m), and coarse enough that a 2 m texture repeat resolves to a few texels.
const float kGrassRootGroundFootprint = 1.0;

// The terrain's own surface normal at the root, for the triplanar projection weights: the same
// textures, decode and resolve the GROUND taps there, including the atlas coarse->slot upgrade
// crossfade cbt_surface.glsl applies to its normal, on the row this resolve read, so the projection
// weights under a blade fade in on the ground's clock (CBTLayoutTests pins the pair).
//
// Called only when a contributing material is textured: an untextured record resolves to its tint
// and never reads the projection weights.
vec3 grassRootTerrainNormalWS(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.NormalmapBindless != 0u)
    {
        vec2 nXZ = textureLod(GE_BTEX(tp.NormalmapBindless, GE_TS_CLAMP),
                              CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(tp.NormalmapBindless,
                                                                                GE_TS_CLAMP), 0))),
                              0.0).rg;
        return normalize(vec3(nXZ.x, sqrt(max(1.0 - dot(nXZ, nXZ), 0.0)), nXZ.y));
    }
    if (grassAtlasBacked(tp.Flags))
    {
        AtlasResolve r = grassAtlasResolve(uv);
        bool haveSlot = r.Resident && Atlas.NormalBindless != 0u;
        bool haveCoarse = Atlas.NormalCoarseBindless != 0u;
        float slotWeight = GrassAtlas_SlotUpgradeWeight(haveSlot, haveCoarse, r.Fade, true);
        vec2 nXZ = vec2(0.0);
        if (haveSlot)
        {
            nXZ = textureLod(GE_BTEX(Atlas.NormalBindless, GE_TS_CLAMP), r.SlotUV, 0.0).rg;
            if (slotWeight < 1.0)
                nXZ = mix(textureLod(GE_BTEX(Atlas.NormalCoarseBindless, GE_TS_CLAMP),
                                     grassCoarseUV(uv, Atlas.CoarseDim), 0.0).rg,
                          nXZ, slotWeight);
        }
        else if (haveCoarse)
            nXZ = textureLod(GE_BTEX(Atlas.NormalCoarseBindless, GE_TS_CLAMP),
                             grassCoarseUV(uv, Atlas.CoarseDim), 0.0).rg;
        return normalize(vec3(nXZ.x, sqrt(max(1.0 - dot(nXZ, nXZ), 0.0)), nXZ.y));
    }
    return vec3(0.0, 1.0, 0.0);
}

// The terrain's resolved albedo around a blade's root. rootRelWS is the root relative to the
// render origin, which the projection UVs are anchored to.
vec3 GrassRootGroundColor(TerrainParamsEntry tp, vec3 rootWS, vec3 rootRelWS)
{
    vec2 uv = clamp(vec2((rootWS.x - tp.WorldOriginX) / max(tp.WorldSizeX, 0.0001),
                         (rootWS.z - tp.WorldOriginZ) / max(tp.WorldSizeZ, 0.0001)),
                    vec2(0.0), vec2(1.0));

    vec4 weights = GrassAtlas_ResolveSplat(tp.Flags, tp.SplatmapBindless, uv, true);
    uint roles[4] = uint[4](tp.LayerRole[0], tp.LayerRole[1], tp.LayerRole[2], tp.LayerRole[3]);
    weights = CBT_ResolveBlendWeights(weights, roles);

    bool anyTextured = false;
    for (int i = 0; i < 4; ++i)
        anyTextured = anyTextured || (weights[i] > 0.0 && TerrainMaterials[roles[i]].AlbedoTex != 0u);
    CBTTriplanarCtx tc = CBT_BuildTriplanarCtx(
        rootRelWS, anyTextured ? grassRootTerrainNormalWS(tp, uv) : vec3(0.0, 1.0, 0.0), uv);
    // The footprint as screen-space-style gradients: x along world X, y along world Y and Z, so
    // each of the three projections (XY, XZ, YZ) sees the footprint on both of its axes.
    float footprint = kGrassRootGroundFootprint;
    tc.dposdx = vec3(footprint, 0.0, 0.0);
    tc.dposdy = vec3(0.0, footprint, footprint);
    tc.dUVdx = vec2(footprint / max(tp.WorldSizeX, 0.0001), 0.0);
    tc.dUVdy = vec2(0.0, footprint / max(tp.WorldSizeZ, 0.0001));
    float uvTiling = CBT_MaterialUVTiling(tp.MaterialTiling);

    vec3 color = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (weights[i] > 0.0)
        {
            TerrainMaterialRecordData mat = TerrainMaterials[roles[i]];
            CBTMaterialUV muv = CBT_MaterialUVs(mat, tc, uvTiling);
            color += CBT_MaterialAlbedo(uint(i), mat, tc, muv, rootWS, footprint) * weights[i];
        }
    }
    return color;
}

#endif // GRASS_ROOT_GROUND_COLOR_GLSL
