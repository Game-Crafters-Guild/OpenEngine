// HZB downsample, tile pass (1 of 2) — the SPD-style collapse of the per-mip
// hzb_build.comp chain into two dispatches (HZBBuildNode selects this path;
// GE_HZB_SPD=0, over-long chains, or a missing shaderpkg fall back).
//
// The body of hzb_spd.comp (the occlusion pyramid) and hzb_spd_nearest.comp
// (the reflection traversal pyramid); Includes/hzb_reduce.glsl says which
// extreme each keeps and why.
//
// CONTENT CONTRACT: pyramid texels are BIT-IDENTICAL to the chain's output
// (HzbSpdParityTests pins this on GPU for both reductions). Same 2x2
// HZB_REDUCE, same trailing odd-extent fold, same operation order.
//
// This pass (workgroup = 16x16 threads over a 64x64 depth tile) builds mips
// 0..6: mip 0 copy + mip 1 with the EXACT fold rule (both read only the depth
// image), then mips 2..6 from an LDS ping-pong with a plain clamped 2x2. A
// valid dest texel's plain 2x2 never reads outside the source texels this
// same workgroup computed (the clamp stays in-tile), so LDS carries every
// intra-dispatch dependency and no storage image is ever re-read — all image
// bindings are pure readonly/writeonly and nothing needs `coherent` (which
// measured ~2x on the dispatch by defeating caching in the single-dispatch
// elected-workgroup variant this replaced).
//
// The odd-extent fold CAN cross tiles, so this pass skips folds for mips
// 2..6. The wrongness is confined to each level's global last row/column
// (a level's last row/column feeds only the NEXT level's last row/column);
// the finalize pass (Includes/hzb_spd_finalize.glsl) repairs exactly those
// texels and builds mips 7+.

#include "Includes/hzb_reduce.glsl"

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, r32f) uniform readonly image2D uDepth;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D uMip0;
// Mips 1..6 as single-mip views (slot i = mip i+1; slots past the real chain
// re-bind the deepest mip and are never addressed — mipCount clamps).
layout(set = 0, binding = 2, r32f) uniform writeonly image2D uMips[6];

layout(push_constant) uniform PushConstants
{
    uint width;    // depth / mip 0 extent
    uint height;
    uint mipCount; // full chain length incl. mip 0
};

// Ping-pong tiles: levels 1/3/5 in sA, levels 2/4/6 in sB.
shared float sA[32][32];
shared float sB[16][16];

ivec2 LevelExtent(uint m)
{
    return ivec2(int(max(1u, width >> m)), int(max(1u, height >> m)));
}

void StoreMip(uint m, ivec2 c, float v) // m in 1..6 -> uMips[m-1]
{
    const vec4 t = vec4(v, 0.0, 0.0, 0.0);
    switch (m)
    {
        case 1u: imageStore(uMips[0], c, t); break;
        case 2u: imageStore(uMips[1], c, t); break;
        case 3u: imageStore(uMips[2], c, t); break;
        case 4u: imageStore(uMips[3], c, t); break;
        case 5u: imageStore(uMips[4], c, t); break;
        default: imageStore(uMips[5], c, t); break;
    }
}

float LoadDepthClamped(ivec2 c, ivec2 maxSrc)
{
    return imageLoad(uDepth, clamp(c, ivec2(0), maxSrc)).r;
}

void main()
{
    const ivec2 wg = ivec2(gl_WorkGroupID.xy);
    const ivec2 lid = ivec2(gl_LocalInvocationID.xy);

    const ivec2 ext0 = ivec2(int(width), int(height));
    const ivec2 maxSrc0 = ext0 - ivec2(1);

    // ── Mip 0 copy + mip 1, from the depth image only ───────────────────────
    // Each thread owns a 4x4 depth block — exactly the interior 2x2 footprints
    // of its four mip-1 texels — loaded once into registers. Clamped loads
    // duplicate edge texels, matching hzb_build.comp's LoadClamped.
    {
        const ivec2 blockBase = wg * 64 + lid * 4;
        float blk[4][4];
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                blk[j][i] = LoadDepthClamped(blockBase + ivec2(i, j), maxSrc0);

        // Mip 0: same-size copy (hzb_build.comp mode 0).
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                const ivec2 c = blockBase + ivec2(i, j);
                if (c.x < ext0.x && c.y < ext0.y)
                    imageStore(uMip0, c, vec4(blk[j][i], 0.0, 0.0, 0.0));
            }

        if (mipCount > 1u)
        {
            const ivec2 ext1 = LevelExtent(1u);
            const bool oddW = (width & 1u) != 0u;
            const bool oddH = (height & 1u) != 0u;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    const ivec2 local = lid * 2 + ivec2(i, j); // 0..31 in-tile
                    const ivec2 d = wg * 32 + local;
                    if (d.x >= ext1.x || d.y >= ext1.y)
                        continue;
                    // Interior 2x2 from registers, hzb_build.comp's op order;
                    // the rare fold texels re-load from the image (tile edge).
                    const int bi = i * 2;
                    const int bj = j * 2;
                    float reduced = blk[bj][bi];
                    reduced = HZB_REDUCE(reduced, blk[bj][bi + 1]);
                    reduced = HZB_REDUCE(reduced, blk[bj + 1][bi]);
                    reduced = HZB_REDUCE(reduced, blk[bj + 1][bi + 1]);

                    const bool lastCol = d.x == ext1.x - 1;
                    const bool lastRow = d.y == ext1.y - 1;
                    const ivec2 base = d * 2;
                    if (oddW && lastCol)
                    {
                        reduced = HZB_REDUCE(reduced, LoadDepthClamped(base + ivec2(2, 0), maxSrc0));
                        reduced = HZB_REDUCE(reduced, LoadDepthClamped(base + ivec2(2, 1), maxSrc0));
                    }
                    if (oddH && lastRow)
                    {
                        reduced = HZB_REDUCE(reduced, LoadDepthClamped(base + ivec2(0, 2), maxSrc0));
                        reduced = HZB_REDUCE(reduced, LoadDepthClamped(base + ivec2(1, 2), maxSrc0));
                    }
                    if (oddW && oddH && lastCol && lastRow)
                        reduced = HZB_REDUCE(reduced, LoadDepthClamped(base + ivec2(2, 2), maxSrc0));

                    StoreMip(1u, d, reduced);
                    sA[local.y][local.x] = reduced;
                }
        }
    }

    // ── Mips 2..6: LDS ping-pong, plain clamped 2x2 (folds deferred to the
    // finalize pass). SRC/DST alternate sA/sB; barriers sit in uniform control
    // flow (the per-level guards are push-constant-uniform).
#define GE_SPD_LDS_LEVEL(dstMip, SRC, DST)                                                     \
    barrier();                                                                                 \
    if (mipCount > uint(dstMip))                                                               \
    {                                                                                          \
        const int span = 64 >> dstMip;                                                         \
        if (lid.x < span && lid.y < span)                                                      \
        {                                                                                      \
            const ivec2 d = wg * span + lid;                                                   \
            const ivec2 dstExt = LevelExtent(uint(dstMip));                                    \
            if (d.x < dstExt.x && d.y < dstExt.y)                                              \
            {                                                                                  \
                const ivec2 srcExt = LevelExtent(uint(dstMip) - 1u);                           \
                const ivec2 maxS = srcExt - ivec2(1);                                          \
                const ivec2 srcTile = wg * (span * 2);                                         \
                const ivec2 b = d * 2;                                                         \
                const ivec2 s00 = clamp(b, ivec2(0), maxS) - srcTile;                          \
                const ivec2 s10 = clamp(b + ivec2(1, 0), ivec2(0), maxS) - srcTile;            \
                const ivec2 s01 = clamp(b + ivec2(0, 1), ivec2(0), maxS) - srcTile;            \
                const ivec2 s11 = clamp(b + ivec2(1, 1), ivec2(0), maxS) - srcTile;            \
                float reduced = SRC[s00.y][s00.x];                                             \
                reduced = HZB_REDUCE(reduced, SRC[s10.y][s10.x]);                              \
                reduced = HZB_REDUCE(reduced, SRC[s01.y][s01.x]);                              \
                reduced = HZB_REDUCE(reduced, SRC[s11.y][s11.x]);                              \
                StoreMip(uint(dstMip), d, reduced);                                            \
                DST[lid.y][lid.x] = reduced;                                                   \
            }                                                                                  \
        }                                                                                      \
    }

    GE_SPD_LDS_LEVEL(2, sA, sB)
    GE_SPD_LDS_LEVEL(3, sB, sA)
    GE_SPD_LDS_LEVEL(4, sA, sB)
    GE_SPD_LDS_LEVEL(5, sB, sA)
    GE_SPD_LDS_LEVEL(6, sA, sB)
#undef GE_SPD_LDS_LEVEL
}
