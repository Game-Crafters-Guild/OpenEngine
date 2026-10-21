#pragma once

#include "Rendering/Core/Device.h"
#include "BackgroundImageGeometry.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace UI
{

// Largest raster edge a UI draw of an SVG asks for. A draw above it samples a
// magnified 4096 px raster rather than baking a larger one.
inline constexpr uint32_t kMaxSvgBackgroundRasterEdge = 4096u;

// The pixel size an SVG background is baked at: the drawn box rounded up to
// whole device pixels on each axis, so the raster is the picture at the size
// it is drawn and samples texel for texel.
struct SvgRasterSize
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    bool operator==(const SvgRasterSize&) const = default;
};

// The raster size for a draw of `drawn`: each axis rounded up to a whole pixel
// and clamped to 1..kMaxSvgBackgroundRasterEdge. A size a float rounding
// error above a whole pixel rounds down to it, so a contain fit that lands on
// 20.000002 px bakes 20 px and not 21.
SvgRasterSize SvgBackgroundRasterSize(const BackgroundImageSize& drawn);

// One GPU raster of an SVG background at one pixel size.
struct SvgBackgroundVariant
{
    SvgRasterSize Size;
    Rendering::TextureHandle Handle = Rendering::INVALID_TEXTURE_HANDLE;
    // Levels uploaded, counted by the cache's byte budget: 1, since a variant
    // is drawn at the size it was baked at.
    uint32_t MipLevels = 1;
    uint64_t LastUsedFrame = 0;
};

// Whether a raster of `baked` may stand in for `wanted` while `wanted` waits
// to bake: within a factor of two on each axis. Further off, a magnified
// raster turns blurry and a minified one drops detail, so the wanted size
// bakes at once.
bool IsSvgRasterSizeCloseEnough(SvgRasterSize baked, SvgRasterSize wanted);

// Index of the baked variant nearest to `size` (summed log ratio of the two
// axes), or `variants.size()` when none is baked. The draw binds it while the
// exact size bakes, so an icon never blinks blank during a resize.
size_t NearestSvgBackgroundVariant(const std::vector<SvgBackgroundVariant>& variants, SvgRasterSize size);

} // namespace UI
} // namespace GameEngine
