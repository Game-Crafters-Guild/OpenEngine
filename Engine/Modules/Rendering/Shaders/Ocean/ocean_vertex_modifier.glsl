// Ocean vertex output modifier for the adapter pipeline.
// Included by adapter_vertex.glsl via GE_VERTEX_MODIFIER_PATH when
// HAS_VERTEX_OUTPUT_MODIFIER is defined.
//
// Declares its own packed clipmap-ring vertex input, follows/snaps the rings to
// the camera, and displaces them from the FFT cascade stack.

#include "Ocean/ocean_common.glsl"
#include "Ocean/ocean_ring_layout.glsl"

// xy = clipmap-local XZ position in base-tile units, z = LOD ring index,
// w = cell-footprint multiplier (the pushed outer horizon skirt is larger).
layout(location = 0) in vec4 aGridData;

const float GE_OCEAN_GEOMETRY_MIN_VERTICAL_WAVE_SIZE = 2.0;
const float GE_OCEAN_GEOMETRY_MIN_HORIZONTAL_WAVE_SIZE = 8.0;

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    vec3 camPos = Cam.uCameraPos.xyz;

    OceanRingVertex ring = OceanPlaceRingVertex(aGridData, camPos, uSeaLevel,
                                                uPatchExtent, uRingCellsPerTile);
    vec2 worldXZ = ring.worldXZ;
    float vertexFootprint = ring.footprint;

    float minVerticalWaveSize = max(GE_OCEAN_GEOMETRY_MIN_VERTICAL_WAVE_SIZE,
                                    vertexFootprint * 2.0);
    float minHorizontalWaveSize = max(GE_OCEAN_GEOMETRY_MIN_HORIZONTAL_WAVE_SIZE,
                                      vertexFootprint * 4.0);

    vec3 pos;
    vec3 nrm;
    if (uFFTCascadeCount > 0u)
    {
        // FFT path: sum the displacement cascades for the position only. The
        // fragment stage builds the FFT normal per pixel (ocean_surface.glsl,
        // macroN) and reads the vertex normal only on the Gerstner path.
        vec3 d0 = OceanShapeDisplacementFilteredAniso(worldXZ, minHorizontalWaveSize, minVerticalWaveSize);
        pos = vec3(worldXZ.x + d0.x, uSeaLevel + d0.y, worldXZ.y + d0.z);
        nrm = vec3(0.0, 1.0, 0.0);
    }
    else
    {
        float foam;
        EvaluateGerstner(worldXZ, pos, nrm, foam);
    }

    // Dynamic waves: add the ripple height and move XZ along its slope (central
    // difference at the undisplaced world XZ, textureLod). Zero when the sim is
    // inactive. The fragment stage tilts its normal by the same slope on both paths.
    if (uDynamicWavesAvailable != 0u)
    {
        float dynScale = OceanSampleWaveMask(worldXZ).x;
        float dynH = clamp(OceanSampleDynWaveHeight(worldXZ) * uDynWavesAmplitude * dynScale,
                           -uDynWavesDisplacementClamp, uDynWavesDisplacementClamp);
        pos.y += dynH;

        float de = 0.5; // world-space slope step (meters)
        float hL = OceanSampleDynWaveHeight(worldXZ - vec2(de, 0.0)) * uDynWavesAmplitude * dynScale;
        float hR = OceanSampleDynWaveHeight(worldXZ + vec2(de, 0.0)) * uDynWavesAmplitude * dynScale;
        float hD = OceanSampleDynWaveHeight(worldXZ - vec2(0.0, de)) * uDynWavesAmplitude * dynScale;
        float hU = OceanSampleDynWaveHeight(worldXZ + vec2(0.0, de)) * uDynWavesAmplitude * dynScale;
        vec2 dynGradient = vec2(hR - hL, hU - hD) / (2.0 * de);
        pos.xz -= dynGradient * uDynWavesHorizontalDisplacement;
        vec3 dynN = normalize(vec3(hL - hR, 2.0 * de, hD - hU));
        // Tilt the vertex normal by the ripple slope. Only the Gerstner path reads
        // it; on the FFT path it tilts the constant up vector and nothing reads it.
        nrm = normalize(nrm + vec3(dynN.x, 0.0, dynN.z));
    }

    // Vertex output modifiers produce WORLD-space position and normal directly.
    v.position = pos;
    v.normal   = nrm;
    v.uv0      = worldXZ;                       // world XZ (meters) for detail UVs
    v.custom0  = vec4(1.0);                     // reserved (flat-interpolated)
}
