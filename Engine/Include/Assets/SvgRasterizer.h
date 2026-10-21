#pragma once

#include "Types/Types.h"

#include <memory>
#include <string>

namespace GameEngine {

struct SvgRasterizedImage
{
    uint32 Width = 0;
    uint32 Height = 0;
    std::unique_ptr<uint8[]> Data; // RGBA8, Width * Height * 4
    size_t DataSize = 0;
};

inline constexpr uint32 kMinSvgRasterSize = 16u;
inline constexpr uint32 kMaxSvgRasterSize = 16384u;

// Parse a decimal per-asset raster size (kSvgRasterSizeMetaKey: the target pixel
// length of the longest axis; the SVG aspect ratio is kept). Valid numeric values
// are clamped to the supported range; malformed or empty values leave the caller's
// fallback intact.
bool ParseSvgRasterSizeMeta(const std::string& value, uint32& outTargetPixels);

/// Read the SVG's resolved source dimensions without rasterizing it. Returns
/// false when the SVG cannot be parsed or ThorVG is unavailable.
bool GetSvgSourceSize(const std::string& svgUtf8,
                      float32& outWidth,
                      float32& outHeight);

/// Calculate the exact raster dimensions for a longest-axis target using the
/// same rounding and clamping rules as RasterizeSvgToRgbaAtSize.
bool CalculateSvgRasterDimensions(float32 sourceWidth,
                                  float32 sourceHeight,
                                  float32 targetPixels,
                                  uint32& outWidth,
                                  uint32& outHeight);

/// Set/get the default longest-axis raster size for SVG texture assets that do
/// not carry a per-asset kSvgRasterSizeMetaKey override. UI draws of an SVG do
/// not use it: they rasterize at the drawn size through
/// RasterizeSvgToRgbaAtDimensions on TextureAsset::SnapshotSvgSource.
void SetSvgTextureRasterizerDefaultSize(float32 targetPixels);
float32 GetSvgTextureRasterizerDefaultSize();

// Rasterize an SVG UTF-8 string into an RGBA8 image buffer using ThorVG with an
// explicit longest-axis target (the aspect ratio is kept). Returns true on
// success and fills outImage; returns false on failure.
bool RasterizeSvgToRgbaAtSize(const std::string& svgUtf8,
                              float32 targetPixels,
                              SvgRasterizedImage& outImage);

// Rasterize an SVG UTF-8 string into a width x height RGBA8 image (each in
// 1..kMaxSvgRasterSize, straight alpha). The picture fills the whole raster; a
// box whose aspect differs from the SVG's stretches the picture on each axis
// (preserveAspectRatio is not honored). Each pixel holds the picture's coverage
// of it: the raster is made at up to 4 x 4 samples per pixel and averaged, so
// crossing strokes unite instead of adding up and a drawn-size icon matches the
// SVG.
bool RasterizeSvgToRgbaAtDimensions(const std::string& svgUtf8,
                                    uint32 width,
                                    uint32 height,
                                    SvgRasterizedImage& outImage);

} // namespace GameEngine
