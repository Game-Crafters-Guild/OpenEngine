// The grass canopy as the ground and the blade both see it: where the field stands (the field
// coverage the placement spawns blades by) and how much light, sun and sky alike, it keeps from the
// lower blade and the ground under it.
//
// ONE definition for three consumers: terrain_grass_place.comp spawns blades by the field coverage,
// cbt_surface.glsl darkens the ground by the coverage at its own point, and terrain_grass_surface.glsl
// darkens the blade base by the coverage its site had at placement (carried in the instance). The
// ground and the blade root take the SAME depth times the SAME coverage, so the contact keeps its
// identity at a feathered splat edge and under a grass region as well as inside a full field: a
// one-sided shade lands as a step at the contact line. Both surfaces hand it to the lighting model as
// the share of the light that reaches them (SurfaceOutput.lightVisibility).
//
// The literals carry the `f` suffix because the block is lifted verbatim into the host tests
// (GlslShim.h), where an unsuffixed literal is a double.

#ifndef GE_TERRAIN_GRASS_CANOPY_GLSL
#define GE_TERRAIN_GRASS_CANOPY_GLSL

// GE_SHARED_GRASS_CANOPY_BEGIN
float GrassSplatLayerWeight(vec4 weights, uint layer)
{
    if (layer == 0u) return weights.x;
    if (layer == 1u) return weights.y;
    if (layer == 2u) return weights.z;
    return weights.w;
}

// The splat feather placement spawns blades by: the grass layer's weight against the mask threshold,
// times its dominance over the strongest other layer. 0 where no blade can stand, 1 in a full
// field. Without a splat the terrain is all channel 0, and the feather is wider.
float GrassSplatCoverage(vec4 splatWeights, uint grassLayer, float maskThreshold, bool hasSplatmap)
{
    float grassWeight = GrassSplatLayerWeight(splatWeights, grassLayer);
    float otherMax = 0.0f;
    for (uint layer = 0u; layer < 4u; ++layer)
    {
        if (layer != grassLayer)
            otherMax = max(otherMax, GrassSplatLayerWeight(splatWeights, layer));
    }

    float threshold = clamp(maskThreshold, 0.0f, 1.0f);
    float dominantGrass = grassWeight - otherMax;
    float dominanceFloor = min(0.18f, threshold);
    if (hasSplatmap && (grassWeight < threshold || dominantGrass < dominanceFloor))
        return 0.0f;

    float maskFeather = hasSplatmap ? 0.08f : 0.25f;
    float dominanceMin = hasSplatmap ? dominanceFloor : 0.08f;
    float dominanceMax = hasSplatmap ? (dominanceFloor + 0.18f) : 0.28f;
    return smoothstep(threshold, min(threshold + maskFeather, 1.0f), grassWeight)
        * smoothstep(dominanceMin, dominanceMax, dominantGrass);
}

// The field coverage at a site: the splat coverage times the authored region density, and nothing
// where a region removes the grass (its height channel at 0). `regional` is the RG8 grass control
// (x = height, y = density); (1, 1) where no control map is bound. The placement spawns a blade
// with exactly this probability.
float GrassFieldCoverage(float splatCoverage, vec2 regional)
{
    return regional.x > 0.0f ? splatCoverage * clamp(regional.y, 0.0f, 1.0f) : 0.0f;
}

// Height fraction over which the canopy occlusion on a blade opens up: full depth at the root, none
// above half the blade, where the blade stands clear of its neighbours.
const float kGrassCanopyOcclusionRange = 0.5f;

// The occlusion on a blade at height fraction t. `depth` is the terrain row's canopy depth
// (TerrainGrassCanopyOcclusionDepth), 0 for a row that places no blade; `coverage` is the field
// coverage at the blade's site. At the root it equals the ground's occlusion at that coverage.
float GrassBladeCanopyOcclusion(float depth, float coverage, float t)
{
    return 1.0f - depth * clamp(coverage, 0.0f, 1.0f)
        * (1.0f - smoothstep(0.0f, kGrassCanopyOcclusionRange, t));
}

// The occlusion on the ground under the field: the blade root's depth, scaled by the field
// coverage at this point.
float GrassGroundCanopyOcclusion(float depth, float coverage)
{
    return 1.0f - depth * clamp(coverage, 0.0f, 1.0f);
}
// GE_SHARED_GRASS_CANOPY_END

#endif // GE_TERRAIN_GRASS_CANOPY_GLSL
