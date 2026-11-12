#include "OceanRingMesh.h"

#include "Mathematics/Interpolation.h"
#include "Mathematics/Vector2.h"

#include <cmath>

namespace GameEngine::Ocean
{

namespace
{

// Concentric patch topology adapted from Wave Harmonic's Crest OceanBuilder
// (MIT, Copyright (c) 2019 Wave Harmonic and contributors; see Ocean/NOTICE).
// Fat/slim patch borders weld adjacent resolutions without cracks.
enum class PatchType : uint8
{
    Interior,
    Fat,
    FatX,
    FatXSlimZ,
    FatXOuter,
    FatXZ,
    FatXZOuter,
    SlimX,
    SlimXZ,
    SlimXFatZ,
};

struct PatchPlacement
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
    PatchType Type = PatchType::Interior;
};

struct PatchSkirts
{
    int32 XMinus = 0;
    int32 XPlus = 0;
    int32 ZMinus = 0;
    int32 ZPlus = 0;
};

constexpr uint32 kGeometryLodCount = 8u;
constexpr float32 kHorizonSkirtMultiplier = 4.0f;

bool IsSidePatch(PatchType type)
{
    return type == PatchType::FatX || type == PatchType::FatXOuter ||
           type == PatchType::SlimX || type == PatchType::SlimXFatZ;
}

bool IsCornerPatch(PatchType type)
{
    return type == PatchType::FatXZ || type == PatchType::SlimXZ ||
           type == PatchType::FatXSlimZ || type == PatchType::FatXZOuter;
}

// Patch variants put their skirt on local +X (and, for corners, +Z). Rotates
// the point toward this tile's side or corner of the ring; .x is X, .y is Z.
Mathematics::Vector2 RotateOutward(float32 x, float32 z, const PatchPlacement& placement)
{
    int quarterTurns = 0;
    if (IsSidePatch(placement.Type))
    {
        if (std::abs(placement.Z) >= std::abs(placement.X))
            quarterTurns = placement.Z > 0.0f ? 3 : 1;
        else if (placement.X < 0.0f)
            quarterTurns = 2;
    }
    else if (IsCornerPatch(placement.Type))
    {
        if (placement.X < 0.0f && placement.Z > 0.0f)
            quarterTurns = 3;
        else if (placement.X < 0.0f && placement.Z < 0.0f)
            quarterTurns = 2;
        else if (placement.X > 0.0f && placement.Z < 0.0f)
            quarterTurns = 1;
    }

    switch (quarterTurns)
    {
    case 1: return Mathematics::Vector2(z, -x);
    case 2: return Mathematics::Vector2(-x, -z);
    case 3: return Mathematics::Vector2(-z, x);
    default: return Mathematics::Vector2(x, z);
    }
}

PatchSkirts ResolvePatchSkirts(PatchType type)
{
    PatchSkirts skirts;
    switch (type)
    {
    case PatchType::Fat:
        skirts.XMinus = skirts.XPlus = skirts.ZMinus = skirts.ZPlus = 1;
        break;
    case PatchType::FatX:
    case PatchType::FatXOuter:
        skirts.XPlus = 1;
        break;
    case PatchType::FatXZ:
    case PatchType::FatXZOuter:
        skirts.XPlus = skirts.ZPlus = 1;
        break;
    case PatchType::FatXSlimZ:
        skirts.XPlus = 1;
        skirts.ZPlus = -1;
        break;
    case PatchType::SlimX:
        skirts.XPlus = -1;
        break;
    case PatchType::SlimXZ:
        skirts.XPlus = skirts.ZPlus = -1;
        break;
    case PatchType::SlimXFatZ:
        skirts.XPlus = -1;
        skirts.ZPlus = 1;
        break;
    default:
        break;
    }
    return skirts;
}

void AppendPatch(OceanRingMesh& mesh, const PatchPlacement& placement, uint32 lodIndex,
                 uint32 cellsPerTile)
{
    const PatchSkirts skirts = ResolvePatchSkirts(placement.Type);
    const uint32 vertsX = static_cast<uint32>(
        static_cast<int32>(cellsPerTile + 1u) + skirts.XMinus + skirts.XPlus);
    const uint32 vertsZ = static_cast<uint32>(
        static_cast<int32>(cellsPerTile + 1u) + skirts.ZMinus + skirts.ZPlus);
    const float32 cell = 1.0f / static_cast<float32>(cellsPerTile);
    const float32 startX = -0.5f - static_cast<float32>(skirts.XMinus) * cell;
    const float32 endX = 0.5f + static_cast<float32>(skirts.XPlus) * cell;
    const float32 startZ = -0.5f - static_cast<float32>(skirts.ZMinus) * cell;
    const float32 endZ = 0.5f + static_cast<float32>(skirts.ZPlus) * cell;
    const float32 lodScale = static_cast<float32>(1u << lodIndex);
    const uint32 vertexBase = static_cast<uint32>(mesh.Vertices.size());
    const bool outerX = placement.Type == PatchType::FatXOuter ||
                        placement.Type == PatchType::FatXZOuter;

    for (uint32 z = 0; z < vertsZ; ++z)
    {
        float32 localZ = Math::Lerp(startZ, endZ,
                                    static_cast<float32>(z) / static_cast<float32>(vertsZ - 1u));
        const bool horizonZ = placement.Type == PatchType::FatXZOuter && z == vertsZ - 1u;
        if (horizonZ)
            localZ *= kHorizonSkirtMultiplier;

        for (uint32 x = 0; x < vertsX; ++x)
        {
            float32 localX = Math::Lerp(startX, endX,
                                        static_cast<float32>(x) / static_cast<float32>(vertsX - 1u));
            const bool horizonX = outerX && x == vertsX - 1u;
            if (horizonX)
                localX *= kHorizonSkirtMultiplier;

            const Mathematics::Vector2 rotated = RotateOutward(localX, localZ, placement);
            mesh.Vertices.emplace_back((placement.X + rotated.x) * lodScale,
                                       (placement.Z + rotated.y) * lodScale,
                                       static_cast<float32>(lodIndex),
                                       horizonX || horizonZ ? kHorizonSkirtMultiplier : 1.0f);
        }
    }

    for (uint32 z = 0; z + 1u < vertsZ; ++z)
    {
        for (uint32 x = 0; x + 1u < vertsX; ++x)
        {
            const uint32 tl = vertexBase + z * vertsX + x;
            const uint32 tr = tl + 1u;
            const uint32 bl = vertexBase + (z + 1u) * vertsX + x;
            const uint32 br = bl + 1u;
            if (((x ^ z) & 1u) == 0u)
                mesh.Indices.insert(mesh.Indices.end(), {tl, bl, br, tl, br, tr});
            else
                mesh.Indices.insert(mesh.Indices.end(), {tl, bl, tr, tr, bl, br});
        }
    }
}

void AppendLod(OceanRingMesh& mesh, uint32 lod, uint32 cellsPerTile)
{
    const bool outermost = lod + 1u == kGeometryLodCount;
    const PatchType leadSide = outermost ? PatchType::FatXOuter : PatchType::SlimX;
    const PatchType trailSide = outermost ? PatchType::FatXOuter : PatchType::FatX;
    const PatchType leadCorner = outermost ? PatchType::FatXZOuter : PatchType::SlimXZ;
    const PatchType trailCorner = outermost ? PatchType::FatXZOuter : PatchType::FatXZ;
    const PatchType topLeftCorner = outermost ? PatchType::FatXZOuter : PatchType::SlimXFatZ;
    const PatchType bottomRightCorner = outermost ? PatchType::FatXZOuter : PatchType::FatXSlimZ;

    // LOD 0 is the full 4 x 4 square; every coarser LOD drops its 2 x 2 center.
    const PatchPlacement placements[] = {
        {-1.5f,  1.5f, topLeftCorner}, {-0.5f,  1.5f, leadSide},
        { 0.5f,  1.5f, leadSide},      { 1.5f,  1.5f, leadCorner},
        {-1.5f,  0.5f, trailSide},     {-0.5f,  0.5f, PatchType::Interior},
        { 0.5f,  0.5f, PatchType::Interior}, {1.5f, 0.5f, leadSide},
        {-1.5f, -0.5f, trailSide},     {-0.5f, -0.5f, PatchType::Interior},
        { 0.5f, -0.5f, PatchType::Interior}, {1.5f, -0.5f, leadSide},
        {-1.5f, -1.5f, trailCorner},   {-0.5f, -1.5f, trailSide},
        { 0.5f, -1.5f, trailSide},     { 1.5f, -1.5f, bottomRightCorner},
    };
    for (const PatchPlacement& placement : placements)
    {
        if (lod > 0u && placement.Type == PatchType::Interior)
            continue;
        AppendPatch(mesh, placement, lod, cellsPerTile);
    }
}

} // namespace

OceanRingMesh BuildOceanRingMesh(uint32 cellsPerTile)
{
    OceanRingMesh mesh;
    const size_t patchCount = 16u + 12u * (kGeometryLodCount - 1u);
    mesh.Vertices.reserve(patchCount * static_cast<size_t>(cellsPerTile + 2u) *
                          static_cast<size_t>(cellsPerTile + 2u));
    mesh.Indices.reserve(patchCount * static_cast<size_t>(cellsPerTile + 1u) *
                         static_cast<size_t>(cellsPerTile + 1u) * 6u);
    for (uint32 lod = 0; lod < kGeometryLodCount; ++lod)
        AppendLod(mesh, lod, cellsPerTile);
    return mesh;
}

} // namespace GameEngine::Ocean
