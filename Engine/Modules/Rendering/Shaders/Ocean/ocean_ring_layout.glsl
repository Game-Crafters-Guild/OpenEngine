// Camera-following concentric ring layout of the ocean surface mesh.
// Pure functions with no buffer reads: the vertex modifier and the layout probe
// (ocean_ring_layout_probe.comp) call the same code.

#ifndef GE_OCEAN_RING_LAYOUT_GLSL
#define GE_OCEAN_RING_LAYOUT_GLSL

const float GE_OCEAN_GEOMETRY_LOD_COUNT = 8.0;
// Smallest root tile in meters. Below this camera height the rings stop
// shrinking; the cell under the camera is this size over the cells per tile.
const float GE_OCEAN_RING_MIN_ROOT_SCALE = 20.0;
// Octaves added before the floor that picks the root tile; far above the
// divide and log2 error (a few 1e-7) and far below a visible height step.
const float GE_OCEAN_RING_OCTAVE_BIAS = 1.0e-4;
// Morph band in ring tiles past the inner edge. The snap moves a ring by up to
// 2 cells, so the inner edge moves out by up to 2 cells and the slim outer edge
// moves in by up to 3 cells. The band stays inside both edges.
const float GE_OCEAN_RING_MORPH_START = 0.15;
const float GE_OCEAN_RING_MORPH_END = 0.85;

struct OceanRingVertex
{
    vec2 worldXZ;     // world XZ in meters after snap and morph
    float footprint;  // cell width this vertex represents, in meters
    float gridWidth;  // unmorphed cell width of the vertex's LOD, in meters
};

// gridData: xy = ring-local XZ in root tiles, z = LOD index, w = skirt footprint
// multiplier (OceanRingMesh). The root tile doubles per octave of camera height,
// and LOD0 morphs onto its coarse grid across the octave. Layout from Crest.
OceanRingVertex OceanPlaceRingVertex(vec4 gridData, vec3 camPos, float seaLevel,
                                     float patchExtent, float cellsPerTile)
{
    float baseTileScale = max(patchExtent * 5.0 / exp2(GE_OCEAN_GEOMETRY_LOD_COUNT), 1.0);
    float viewerHeight = max(abs(camPos.y - seaLevel), GE_OCEAN_RING_MIN_ROOT_SCALE);
    float altitudeLevel = log2(viewerHeight / baseTileScale);
    // The divide and log2 may land a few ULP under an exact power of two; the
    // bias keeps that height in its own octave instead of halving the root tile.
    float octave = floor(altitudeLevel + GE_OCEAN_RING_OCTAVE_BIAS);
    float meshScaleLerp = max(altitudeLevel - octave, 0.0);
    float rootScale = baseTileScale * exp2(octave);

    float lodIndex = clamp(gridData.z, 0.0, GE_OCEAN_GEOMETRY_LOD_COUNT - 1.0);
    float lodScale = exp2(lodIndex);
    float cells = max(cellsPerTile, 8.0);
    float gridWidth = rootScale * lodScale / cells;

    vec2 worldXZ = camPos.xz + gridData.xy * rootScale;
    float snapWidth = 2.0 * gridWidth;
    worldXZ -= fract(camPos.xz / snapWidth) * snapWidth;

    float taxicab = max(abs(worldXZ.x - camPos.x), abs(worldXZ.y - camPos.z));
    float lodAlpha = taxicab / max(rootScale * lodScale, 1.0e-4) - 1.0;
    float morphStart = max(GE_OCEAN_RING_MORPH_START, 2.0 / cells);
    float morphEnd = min(GE_OCEAN_RING_MORPH_END, 1.0 - 3.0 / cells);
    lodAlpha = clamp((lodAlpha - morphStart) / (morphEnd - morphStart), 0.0, 1.0);
    if (lodIndex < 0.5)
        lodAlpha = min(lodAlpha + meshScaleLerp, 1.0);

    // Odd vertices slide onto the 2x grid as lodAlpha goes from 0 to 1.
    float coarseWidth = 4.0 * gridWidth;
    vec2 coarseCell = fract(worldXZ / coarseWidth) - vec2(0.5);
    if (abs(coarseCell.x) < 0.375)
        worldXZ.x += coarseCell.x * lodAlpha * coarseWidth;
    if (abs(coarseCell.y) < 0.375)
        worldXZ.y += coarseCell.y * lodAlpha * coarseWidth;

    OceanRingVertex result;
    result.worldXZ = worldXZ;
    result.footprint = mix(gridWidth, 2.0 * gridWidth, lodAlpha) * max(gridData.w, 1.0);
    result.gridWidth = gridWidth;
    return result;
}

#endif // GE_OCEAN_RING_LAYOUT_GLSL
