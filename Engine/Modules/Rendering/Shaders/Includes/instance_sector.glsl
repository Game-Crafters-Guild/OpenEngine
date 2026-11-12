// The render-origin sector of an instance, and points local to it. Stand-alone: it needs no
// binding, so compute kernels and the vertex stage share it.
// C++ mirror: Rendering/Core/GPUScene.h (PackSector, UnpackSector, SectorLocalPointRelativeTo).

#ifndef GE_INSTANCE_SECTOR_GLSL
#define GE_INSTANCE_SECTOR_GLSL

// Meters per sector, equal to Mathematics::kWorldSectorSize (CBTLayout.GlslSectorConstantsMatchWorldSectorSize).
const float GE_SECTOR_SIZE = 1024.0;

// Decode the packed 21-bit-per-axis sector from the two GPUInstance words. bitfieldExtract
// sign-extends the 21-bit two's-complement axis.
ivec3 ge_UnpackInstanceSector(uint p0, uint p1)
{
    uint ux = p0 & 0x1FFFFFu;
    uint uy = ((p0 >> 21) & 0x7FFu) | ((p1 & 0x3FFu) << 11);
    uint uz = (p1 >> 10) & 0x1FFFFFu;
    return ivec3(bitfieldExtract(int(ux), 0, 21),
                 bitfieldExtract(int(uy), 0, 21),
                 bitfieldExtract(int(uz), 0, 21));
}

// A point local to the sector packed in (p0, p1), moved to the frame of the origin of
// `originSector`. The integer sectors subtract first, so a near point stays exact at any distance.
vec3 ge_SectorRelativePoint(vec3 local, uint p0, uint p1, ivec3 originSector)
{
    return vec3(ge_UnpackInstanceSector(p0, p1) - originSector) * GE_SECTOR_SIZE + local;
}

#endif // GE_INSTANCE_SECTOR_GLSL
