#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>

namespace GameEngine
{
class TextureAsset;

namespace UI
{

// Upload a texture asset's own pixels as an RGBA8 UI texture with the mip chain
// the UI sampler reads (images up to 1024 px on an edge). 8-bit R/RG/RGB are
// widened to RGBA, float images are tone-mapped to 8 bits, and block-compressed
// payloads are re-decoded from the asset's CPU-readable file. `outMipLevels`
// reports the levels uploaded; an invalid handle means the asset cannot be drawn.
Rendering::TextureHandle UploadBackgroundImageFromAsset(Rendering::IDevice* device, const TextureAsset* texA,
                                                        uint32_t& outMipLevels);

} // namespace UI
} // namespace GameEngine
