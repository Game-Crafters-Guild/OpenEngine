#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Utils/CubeLutGpuBinding.h"
#include "Rendering/Utils/CubeLutFileParser.h"

#include <string>

namespace GameEngine::Rendering
{

struct CubeLutGpuTextures
{
    TextureHandle Lut3D{};
    TextureHandle Lut1DStrip{};
};

bool CreateCubeLutGpuTextures(IDevice* device,
                              const CubeLutParseResult& parsed,
                              TextureFormat textureFormat,
                              CubeLutGpuTextures& outTextures,
                              std::string* outError = nullptr);

/// The identity tables (a 2x2x2 3D table and a 2-entry shaper strip) a LUT binding falls back to,
/// so a pass that samples a LUT always has valid textures. Float16, because the knots are exactly
/// 0 and 1 and float16 filters on every device. The caller owns and destroys both textures.
CubeLutGpuTextures CreateIdentityCubeLutTextures(IDevice* device);

/// The binding a LUT slot holds when there is no usable LUT (none set, or a file that failed to
/// load): the identity tables stay bound so the descriptors are valid, and the flags and sizes
/// tell the shader to sample neither, so the colour passes through unchanged.
CubeLutGpuBindingState IdentityCubeLutBinding(const CubeLutGpuTextures& identity);

} // namespace GameEngine::Rendering
