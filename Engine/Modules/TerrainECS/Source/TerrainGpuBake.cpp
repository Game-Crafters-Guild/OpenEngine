#include "TerrainECS/TerrainGpuBake.h"

#include <cstdlib>

namespace GameEngine::TerrainECS
{

namespace
{
// Test override for the GPU height-bake gate: -1 = follow the env var (default), 0 = force off,
// 1 = force on. TU-local so it is a single instance behind the exported accessors, never a
// cross-dll DATA symbol (which the auto-export does not carry reliably). Stays -1 in shipped runs.
int g_GpuHeightBakeTestOverride = -1;
} // namespace

bool IsGpuHeightBakeEnabled()
{
    if (g_GpuHeightBakeTestOverride >= 0)
        return g_GpuHeightBakeTestOverride != 0;
    // Default ON (kill switch GE_TERRAIN_GPU_BAKE=0). This is inert unless the terrain engaged the atlas
    // (past the unified-source ceiling) AND is whole-resident — AtlasGpuBakeEligible is only set
    // inside the atlas dispatch block — so a terrain on the unified path is unaffected; where the
    // atlas is active it moves the drag bake onto the GPU (parity-proven at the RG16F/RGBA8
    // floors, editor-verified radius-100 drag).
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_GPU_BAKE");
        return !(v != nullptr && v[0] == '0');
    }();
    return enabled;
}

void SetGpuHeightBakeEnabledForTests(int state) { g_GpuHeightBakeTestOverride = state; }

const char* GpuHeightBakeRefusalName(GpuHeightBakeRefusalReason reason)
{
    switch (reason)
    {
    case GpuHeightBakeRefusalReason::None:                  return "none";
    case GpuHeightBakeRefusalReason::Disabled:              return "disabled (GE_TERRAIN_GPU_BAKE=0)";
    case GpuHeightBakeRefusalReason::TextureLayout:         return "the height texture is not a whole-terrain atlas";
    case GpuHeightBakeRefusalReason::BaseSource:            return "the base is not procedural noise";
    case GpuHeightBakeRefusalReason::SplineShape:           return "spline shape";
    case GpuHeightBakeRefusalReason::GlobalVolume:          return "global volume";
    case GpuHeightBakeRefusalReason::InwardFeatherOrWeight: return "Falloff Inward or Weight";
    case GpuHeightBakeRefusalReason::PooledBlend:           return "Average blend";
    case GpuHeightBakeRefusalReason::HeightFromVolume:      return "Relative To Volume";
    case GpuHeightBakeRefusalReason::Erosion:               return "erosion";
    case GpuHeightBakeRefusalReason::HeightOffset:          return "height offset";
    case GpuHeightBakeRefusalReason::Stamp:                 return "stamp";
    case GpuHeightBakeRefusalReason::GroundClaim:           return "ground claim";
    case GpuHeightBakeRefusalReason::OtherEffect:           return "an effect kind the GPU bake has no code for";
    case GpuHeightBakeRefusalReason::SculptZone:            return "sculpt zone";
    }
    return "unknown";
}

const char* TerrainHeightBakePathName(TerrainHeightBakePath path)
{
    switch (path)
    {
    case TerrainHeightBakePath::Gpu:      return "gpu";
    case TerrainHeightBakePath::Cpu:      return "cpu";
    case TerrainHeightBakePath::NotBaked: return "not baked";
    }
    return "unknown";
}

} // namespace GameEngine::TerrainECS
