#pragma once

#include "TerrainECS/TerrainFieldData.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

struct ResolvedModifier;
struct TerrainGrassField;

// Bytes per colour-map texel: the premultiplied colour and the weight.
inline constexpr uint32 kTerrainColorMapChannels = 4u;

// The terrain's colour map: one RGBA8 texel per terrain sample, on the splat's lattice.
//
// rgb is the ground's final colour times its weight, in linear light, stored sRGB-encoded so the
// dark steps keep their precision; the GPU image is RGBA8_SRGB and the sampler decodes it. a is
// the weight, linear. The ground reads ground x (1 - a) + rgb. Premultiplied, so the bilinear
// filter between a written texel and an unwritten one gives the half mix, not a darker colour.
//
// A zero texel is identity. Empty Pixels means no writer reaches the terrain: identity everywhere,
// and no GPU texture. The dimensions still record the lattice the map was composed on.
struct TerrainColorMap : TerrainFieldData
{
    // The writers this content was composed from (TerrainColorMapBaker); 0 before the first compose.
    uint64 InputsStamp = 0;
};

// The grass's ground write: Soil Color at the weight Amount x the region's Darken Ground scale x
// the field coverage, where the terrain's grass places blades. Off when the terrain has no enabled
// grass that places blades, or Darken Ground is 0.
struct TerrainColorGroundWrite
{
    bool Enabled = false;
    float32 Amount = 0.0f;                            // TerrainGrass::GroundDarkening, [0, 1]
    float32 SoilLinear[3] = {0.0f, 0.0f, 0.0f};       // TerrainGrass::SoilColor, decoded to linear
    uint32 GrassLayer = 0;                            // the splat channel the grass grows on
    float32 MaskThreshold = 0.45f;
};

// The splat a compose reads: RGBA8 channel weights on the compose's own lattice (Width x Height
// equal to the inputs'), or on a coarser lattice over the same rect, read bilinearly. Null Texels
// with HasSplatmap means the terrain has a splat that holds nothing here (all zero, read as
// channel 0, as the GPU reads an unwritten splat); without HasSplatmap the terrain has no splat.
struct TerrainColorSplatSource
{
    const uint8* Texels = nullptr;
    uint32 Width = 0;
    uint32 Height = 0;
    bool HasSplatmap = false;
};

// The grass control field a compose reads the region scales from, and the world rect its lattice
// spans. A null field reads 1 for every scale.
struct TerrainColorGrassSource
{
    const TerrainGrassField* Field = nullptr;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
};

// The inputs of one compose: the lattice, where it lies in the world, and the writers, in the
// order they compose (each over what is already there): the ground write, then the zones.
// A zone is a colour zone's payload or a volume's Color effect.
struct TerrainColorComposeInputs
{
    // The lattice: Width x Height samples spanning [OriginX, OriginX + SizeX] x
    // [OriginZ, OriginZ + SizeZ], sample 0 on the origin and sample Width - 1 on the far edge.
    uint32 Width = 0;
    uint32 Height = 0;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    TerrainColorSplatSource Splat;
    TerrainColorGrassSource Grass;
    TerrainColorGroundWrite Write;
    // Colour zones (ResolvedModifier::Type::ColorZone) and volumes carrying exactly one Color effect,
    // in priority order, lowest first.
    std::span<const ResolvedModifier> Zones;
};

// Recompose the inclusive texel rect [minX, maxX] x [minZ, maxZ] from identity through every
// writer; a negative max means the whole map. Allocates the map when a writer reaches the lattice
// and empties it when none does. Bumps Version and grows the dirty rect when a texel changes.
void ComposeTerrainColorMap(TerrainColorMap& map, const TerrainColorComposeInputs& inputs, int32 minX = 0,
                            int32 minZ = 0, int32 maxX = -1, int32 maxZ = -1);

} // namespace GameEngine::TerrainECS
