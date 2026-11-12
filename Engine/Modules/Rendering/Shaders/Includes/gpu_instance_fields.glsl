// GPUInstance std430 field list — the single GLSL source for the C++
// GPUScene.h::GPUInstance mirror (240 B stride, static_assert-locked on the
// C++ side). Included INSIDE a struct body:
//
//     struct GPUInstance {
//     #include "Includes/gpu_instance_fields.glsl"   // from Shaders/*.comp
//     };
//
// (instance_io.glsl includes it as "gpu_instance_fields.glsl" — same dir.)
// Field order and types are ABI: any change here must change GPUScene.h and
// its static_asserts in the same commit.
    mat4 transform;
    mat4 prevTransform;
    // Precomputed normal matrix = transpose(inverse(mat3(transform))),
    // columns in xyz (the first spare word stores layers); CPU-side fill keeps
    // mat3 assembly.
    vec3 normalMatrixCol0;
    uint renderLayerMask;
    vec4 normalMatrixCol1;
    vec4 normalMatrixCol2;
    uint meshIndex;
    uint materialIndex;
    uint flags;             // bit 0 cast shadows, bit 1 receive shadows,
                            // bit 2 depth material-dependent, bit 3 depth double-sided
                            // (meaningful only when bit 2 clear), bit 4 mirrored winding
                            // (negative-determinant world transform), bits 16..31 = 16-bit world tag
    float lodBias;          // log2 coverage scale consumed by ge_SelectLOD
    vec3 boundingCenter;    // local to the sector in sectorPacked; ge_SectorRelativePoint composes it
    float boundingRadius;
    uint skinPaletteOffset;
    uint runtimeId;         // SkeletonStore runtime id (per-runtime visibility OR)
    // The sector of the transform column and boundingCenter, 21 bits per axis (PackSector in GPUScene.h).
    // Zero is sector (0,0,0), where both are world space. Decode with ge_UnpackInstanceSector.
    uint sectorPacked0;
    uint sectorPacked1;
    vec4 custom0;
