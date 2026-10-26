#pragma once

#include "TerrainECS/TerrainFieldData.h"
#include "Types/Types.h"
#include <vector>
#include <span>

namespace JobSystem { class WorkStealingThreadPool; }

namespace GameEngine::TerrainECS
{

// Channels a control texel carries, in order: height target, density target.
// The GPU side is R8G8_UNORM, so this is also the CPU bytes per texel. Both
// channels are continuous and sampled through a LINEAR clamp sampler — a future
// discrete channel (a grass profile index) cannot simply join them here, because
// filtering would blend two indices into a third that names nothing.
inline constexpr uint32 kTerrainGrassFieldChannels = 2u;

// Derived grass control data. RG8 stores height/density targets, with 255/255
// exactly neutral. Empty Pixels means a neutral field and needs no GPU texture.
// IsActive answers for the uploaded RG8 only; GroundDarkening is never uploaded.
// The terrain compositor owns these bytes; neither height nor splat versions
// change when they change. Dimensions also record a neutral tile's bake lattice.
struct TerrainGrassField : TerrainFieldData
{
    // The composed Darken Ground scale of the regions (TerrainGrassEffect::GroundDarkeningScale),
    // R8, 255 is 1. CPU only: the colour map's ground write reads it at bake and nothing uploads
    // it. Empty means 1 everywhere. On the same lattice as Pixels, and its changes move Version.
    std::vector<uint8> GroundDarkening;
    uint64 AppliedHash = 0;
    uint64 NonNeutralTexels = 0;
    uint64 NonNeutralDarkeningTexels = 0;

    // True when grass placement on its terrain must wait for this field on the GPU: an active
    // field, or one whose first compose has not landed, which would otherwise read as neutral.
    bool GatesPlacement() const { return !Initialized || IsActive(); }
};

struct ResolvedModifier;

// Re-evaluate an inclusive sample rectangle from neutral through the ordered
// targets. Negative maxima mean the full field. No height/splat data is read.
// With `jobs`, a rectangle as large as the parallel ground bake's threshold is
// composed in row bands across the pool, the ground bake's banding (legal from a
// pool worker); the bytes are those of the serial walk.
void ComposeTerrainGrassField(TerrainGrassField& field, uint32 width, uint32 height,
                              float32 sizeX, float32 sizeZ, float32 originX, float32 originZ,
                              std::span<const ResolvedModifier> modifiers,
                              int32 minX = 0, int32 minZ = 0, int32 maxX = -1, int32 maxZ = -1,
                              JobSystem::WorkStealingThreadPool* jobs = nullptr);

} // namespace GameEngine::TerrainECS
