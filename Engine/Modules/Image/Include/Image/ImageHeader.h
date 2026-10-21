#pragma once

#include <cstdint>
#include <filesystem>

namespace GameEngine::Image
{

/// An image file's extent and channels, read from its header without decoding.
struct ImageHeader
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    /// The file's own channels, as DecodeChannels::FromFile decodes them: 1 gray,
    /// 2 gray and alpha, 3 RGB, 4 RGBA. A palette PNG the decoder cannot read the
    /// rest of reports 3.
    uint32_t Channels = 0;
    /// False when the file cannot be opened or is not an image the decodes read. A PNG
    /// is judged by its IHDR: one that declares more than the decoder reads is valid,
    /// with its declared extent and WithinDecoderLimits false.
    bool Valid = false;
    /// False when the header declares more than stb_image's decoder reads, so every
    /// decode refuses the file by name. For a PNG: a side past 2^24 pixels, more than
    /// 2^30 samples (width * height * channels, a palette counted as 4), or image rows
    /// of more than 2^31 - 1 bytes (a 16-bit grayscale PNG holds at most 32768 x 32766).
    bool WithinDecoderLimits = false;
};

/// Reads the header of the image file at `file` (any format the decodes in
/// Image/Decode.h read). It reads the file only as far as the header (for a PNG, up
/// to the first image data chunk), so a listing can call it for every image it shows.
/// Allocates no pixels and applies no budget. Thread-safe.
ImageHeader ReadImageHeader(const std::filesystem::path& file);

} // namespace GameEngine::Image
