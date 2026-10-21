#pragma once

#include "Image/PixelImage.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace GameEngine::Image
{

/// The channels a decode returns. A file with other channels is converted: gray
/// expands to R = G = B, a missing alpha becomes opaque, color converts to gray by
/// luma and alpha is dropped.
enum class DecodeChannels : uint8_t
{
    /// The file's own channels (1 gray, 2 gray and alpha, 3 RGB, 4 RGBA; a palette
    /// image decodes as RGB or RGBA).
    FromFile,
    /// One gray channel.
    Gray,
    /// R, G, B.
    Rgb,
    /// R, G, B, A.
    Rgba,
};

// The three decodes below read a compressed image file held in memory (PNG, JPEG, TGA,
// BMP, PSD, GIF's first frame, HDR and PNM, through stb_image), top row first. Each is
// thread-safe: the module never flips an image and never changes stb_image's
// process-wide settings (the vertical flip, the HDR gamma), and no other code calls
// stb_image. The header is read first, and a decode stb_image cannot produce is
// refused before any pixel is allocated, with a warning that names `sourceName` and the
// limit: input of more than 2^31 - 1 bytes, a PNG past what stb_image's PNG decoder
// reads (see ImageHeader::WithinDecoderLimits), or a result of more than 2^31 - 1
// bytes. There is no smaller cap: a source's size is the author's choice. Bytes that
// are not an image return an invalid result without a log; bytes whose header reads
// but whose data does not decode warn once with the name and stb_image's reason. The
// caller states what it was loading.

/// Decodes to 8 bits per sample. A 16-bit file keeps the high byte of each sample;
/// an HDR file's color is clamped to [0, 1] and encoded with gamma 1/2.2.
/// Largest result: 2^31 - 1 bytes (an RGBA image of about 23170 x 23170).
/// @param fileBytes The file's bytes.
/// @param sourceName The file or asset the bytes came from, for the warning.
/// @param channels The channels to return; see DecodeChannels.
Image8 Decode8(std::span<const uint8_t> fileBytes, std::string_view sourceName, DecodeChannels channels);

/// Decodes to 16 bits per sample. An 8-bit file's samples are widened (v * 257), so
/// 255 becomes 65535. Largest result: 2^31 - 1 bytes; a 16-bit grayscale PNG
/// heightmap holds at most 32768 x 32766, the PNG decoder's limit.
/// @param fileBytes The file's bytes.
/// @param sourceName The file or asset the bytes came from, for the warning.
/// @param channels The channels to return; see DecodeChannels.
Image16 Decode16(std::span<const uint8_t> fileBytes, std::string_view sourceName, DecodeChannels channels);

/// Decodes to linear 32-bit floats. An HDR file keeps its values. Any other file is
/// decoded as Decode8 does, then its color is linearized with gamma 2.2 to [0, 1] and
/// its alpha is scaled to [0, 1].
/// Largest result: 2^31 - 1 bytes (an RGB image of about 13377 x 13377).
/// @param fileBytes The file's bytes.
/// @param sourceName The file or asset the bytes came from, for the warning.
/// @param channels The channels to return; see DecodeChannels.
ImageFloat DecodeFloat(std::span<const uint8_t> fileBytes, std::string_view sourceName, DecodeChannels channels);

} // namespace GameEngine::Image
