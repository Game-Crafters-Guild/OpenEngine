#pragma once

#include "Rendering/Core/Handle.h"

#include <cstdint>

namespace GameEngine::Rendering
{

/// The per-material deformation margins both culling kernels bind
/// (frustum_culling.comp binding 3, hzb_culling.comp binding 6): one float per
/// GPUInstance::materialIndex, in local meters, in a range of the frame's upload
/// ring. A kernel tests a sphere of radius R + margin x largest axis scale
/// (before cullMargins) for a material whose margin is above zero, and R exactly
/// for every other one.
struct CullDeformationMargins
{
    BufferHandle Buffer{};
    uint64_t Offset = 0;
    uint64_t Bytes = 0;
    bool Valid() const { return Buffer.IsValid() && Bytes != 0; }
};

} // namespace GameEngine::Rendering
