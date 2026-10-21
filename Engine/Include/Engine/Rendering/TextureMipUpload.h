#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>

namespace GameEngine
{

struct TextureCookSettings;

// Creates a persistent 2D texture from tightly packed RGBA8 pixels with a FULL mip
// chain and uploads every level. A texture that arrives without a cooked container
// still has to minify through trilinear levels, or it aliases into sparkle (a
// surface) or stair steps (a UI icon drawn smaller than its raster). The chain is
// built by the cook's own builder (BuildTextureCookMipChain) under `cookSettings`,
// so a raw upload filters like the cooked artifact that may later replace it.
// `srgb` selects the builder's sRGB, alpha-weighted filter; `gpuFormat` must be an
// RGBA8 format. Returns an invalid handle when the device refuses the texture or an
// alpha-coverage chain fails; `outLevels` reports the levels uploaded (1 when no
// resampler is compiled in).
Rendering::TextureHandle UploadRgba8WithMips(Rendering::IDevice* device,
                                             const uint8_t* rgba, uint32_t width, uint32_t height,
                                             Rendering::TextureFormat gpuFormat,
                                             const TextureCookSettings& cookSettings,
                                             bool srgb, const char* debugName,
                                             const char* stagingName, uint32_t& outLevels);

} // namespace GameEngine
