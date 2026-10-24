// CBT vertex output modifier (graphics side of the CBT/LEB terrain renderer).
// Included by adapter_vertex.glsl via GE_VERTEX_MODIFIER_PATH when the material's
// customVertexShader=true (which the clamp turns into HAS_VERTEX_OUTPUT_MODIFIER +
// CUSTOM_VERTEX_SHADER). There is NO vertex buffer: geometry is decoded entirely
// from the CBT SSBOs the compute update wrote this frame.
//
// Draw contract (plan §8 C3/C6): the camera forward pass issues DrawIndexedIndirectCount
// over the VISIBLE bisector stream (Classify frustum-culls into it) with an identity
// index buffer, so gl_VertexIndex walks 0..(3*visibleBisectors-1). Each run of three
// consecutive vertex ids is one bisector triangle:
//   triangle = gl_VertexIndex / 3     -> bisector slot via IndicesVisible[triangle]
//   corner   = gl_VertexIndex % 3     -> which of the LEB corners (0,1,2)
//
// C4: the compute VertexEval already displaced each corner to its real WORLD
// position (height sampled from the SAME terrain heightmap CDLOD samples) and stored
// the terrain UV alongside. Those cached corners are camera-independent, so the update
// keeps them while the camera moves. The draw then moves a far planar vertex to the
// prefiltered height its distance asks for (CBT_DrawnCornerHeight, cbt_height_chain.glsl):
// the surface at the detail the screen can show there, so far facets stop aliasing the
// level-0 relief. Near vertices keep the cached corner bit for bit.
//
// Set 2 (the DrawBindings route terrain uses): CBT_DESCRIPTOR_SET=2 puts the draw's
// buffers on set 2, resolved by the MaterialBinder from DrawBindings by instance
// name (gIdxVis / gVertex, and gCbtChain / gCbtDrawLevel outside the compat profile). CBT_GRAPHICS_INCLUDE keeps the include int64-free and
// strips every compute-only write/atomic helper (plan §9 silent-null-variant trap).
#define CBT_GRAPHICS_INCLUDE
#define CBT_DESCRIPTOR_SET 2
#include "cbt_layout.glsl"

#if defined(CBT_DRAW_HEIGHT_CHAIN)
// The height a planar corner at world `pos` and terrain `uv` draws at: the chain level of its distance
// from the CBT camera, or the cached height `pos.y` where the rule asks for level 0, the chain is off
// (scale 0: the sphere, the atlas and paged arms, the narrow heap) or the chain was built for another
// lattice than the bound one. A pure function of (uv, pos, the frame's eye), so every facet that
// shares the corner draws it at one height.
float CBT_DrawnCornerHeight(vec3 pos, vec2 uv)
{
    CBTDrawLevelParamsData p = gCbtDrawLevel.Params;
    if (!(p.eye.w > 0.0) || gCbtChain.Words[0] != p.lattice.x)
        return pos.y;
    precise float d = distance(pos, p.eye.xyz);
    float lambda = CBT_ChainLevel(d, p.eye.w);
    return CBT_ChainDrawnHeight(uv.x, uv.y, pos.y, lambda, p.lattice.x & 0xFFFFu, p.lattice.x >> 16u,
                                gCbtChain.Words[1], p.height.x, p.height.y);
}
#endif

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    uint vid = uint(gl_VertexIndex);
    uint triangle = vid / 3u;
    uint corner = vid - triangle * 3u;

    uint slot = gIdxVis.IndicesVisible[triangle];
    CBTVertexData vd = gVertex.Vertex[slot];

    // This vertex's position + terrain UV (corner.w = uv.x, meta[k] = uv.y). The compute
    // VertexEval already displaced the position (height baked in). The per-pixel normal is
    // sampled from the terrain normalmap in the surface (meta.w = its bindless index),
    // matching CDLOD — smooth + crack-free (plan §8 C4).
    vec3 pos;
    vec2 uv;
    uvec2 packedSector;
    if (corner == 0u)      { pos = vd.corner0.xyz; uv = vec2(vd.corner0.w, vd.meta.x); packedSector = vd.sector0; }
    else if (corner == 1u) { pos = vd.corner1.xyz; uv = vec2(vd.corner1.w, vd.meta.y); packedSector = vd.sector1; }
    else                   { pos = vd.corner2.xyz; uv = vec2(vd.corner2.w, vd.meta.z); packedSector = vd.sector2; }

    // Storage mode rides the per-slot deepTag (terrain S2a). Legacy slots (0): pos is the
    // fp32 WORLD position and the adapter takes the GE_ClipFromWorld path — byte-identical
    // to the pre-S2a draw (dark-ship). Deep slots (1): pos is the SECTOR-LOCAL offset and
    // this corner's integer world sector rides v.sector, so the adapter projects through
    // GE_ClipFromSectorLocal — the exact-integer-delta reconstruction that keeps Earth-
    // magnitude geometry fp32-precise in clip space.
    v.position = pos;
    if (vd.deepTag.x != 0u)
    {
        v.sector = CBT_UnpackCornerSector(packedSector);
        v.useSector = true;
    }
#if defined(CBT_DRAW_HEIGHT_CHAIN)
    else
    {
        v.position.y = CBT_DrawnCornerHeight(pos, uv);
    }
#endif
    v.normal = vec3(0.0, 1.0, 0.0);              // placeholder; the surface samples the normalmap
    v.uv0 = uv;                                  // terrain UV: normalmap + future splat lookups
    v.custom0 = vec4(float(slot), 0.0, 0.0, vd.meta.w); // .x = slot (debug), .w = normalmap index
}
