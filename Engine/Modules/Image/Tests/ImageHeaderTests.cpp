#include "Image/ImageHeader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine;

namespace
{
std::filesystem::path WriteTempFile(const std::string& name, const std::string& contents)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return path;
}

std::string BigEndian32(uint32_t value)
{
    return {static_cast<char>(value >> 24), static_cast<char>(value >> 16), static_cast<char>(value >> 8),
            static_cast<char>(value)};
}

// The signature and IHDR of a 16-bit grayscale PNG, then an empty IDAT: the header alone.
std::string Gray16PngHeader(uint32_t width, uint32_t height)
{
    std::string png("\x89PNG\r\n\x1a\n", 8);
    png += BigEndian32(13) + "IHDR" + BigEndian32(width) + BigEndian32(height);
    png += std::string("\x10\x00\x00\x00\x00", 5); // 16 bits, gray, deflate, no filter, no interlace
    png += BigEndian32(0) + BigEndian32(0) + "IDAT" + BigEndian32(0);
    return png;
}
} // namespace

// The header gives the extent and the file's own channels; the declared pixels are
// never read (the file holds none).
TEST(ImageHeader, ReadsTheExtentAndChannelsFromTheHeaderAlone)
{
    const std::filesystem::path pgm = WriteTempFile("ge-image-header-gray.pgm", "P5\n3000 2000\n255\n");
    const Image::ImageHeader header = Image::ReadImageHeader(pgm);
    EXPECT_TRUE(header.Valid);
    EXPECT_EQ(header.Width, 3000u);
    EXPECT_EQ(header.Height, 2000u);
    EXPECT_EQ(header.Channels, 1u);
    EXPECT_TRUE(header.WithinDecoderLimits);
    std::filesystem::remove(pgm);

    const std::filesystem::path text = WriteTempFile("ge-image-header-text.png", "not an image");
    EXPECT_FALSE(Image::ReadImageHeader(text).Valid);
    std::filesystem::remove(text);
    EXPECT_FALSE(Image::ReadImageHeader(std::filesystem::temp_directory_path() / "ge-image-header-missing.png").Valid);
}

// A 16-bit grayscale PNG's image rows must fit in 2^31 - 1 bytes: 32768 x 32766 is within
// what the decoder reads, 32768 x 32767 is past it. Both report their declared extent.
TEST(ImageHeader, ReportsWhetherAPngIsWithinWhatTheDecoderReads)
{
    const std::filesystem::path fits = WriteTempFile("ge-image-header-32766.png", Gray16PngHeader(32768, 32766));
    const Image::ImageHeader within = Image::ReadImageHeader(fits);
    EXPECT_TRUE(within.Valid);
    EXPECT_TRUE(within.WithinDecoderLimits);
    EXPECT_EQ(within.Width, 32768u);
    EXPECT_EQ(within.Height, 32766u);
    std::filesystem::remove(fits);

    const std::filesystem::path past = WriteTempFile("ge-image-header-32767.png", Gray16PngHeader(32768, 32767));
    const Image::ImageHeader beyond = Image::ReadImageHeader(past);
    EXPECT_TRUE(beyond.Valid);
    EXPECT_FALSE(beyond.WithinDecoderLimits);
    EXPECT_EQ(beyond.Width, 32768u);
    EXPECT_EQ(beyond.Height, 32767u);
    std::filesystem::remove(past);

    // Past stb_image's header limit (2^30 samples): still the declared extent.
    const std::filesystem::path huge = WriteTempFile("ge-image-header-huge.png", Gray16PngHeader(40000, 40000));
    const Image::ImageHeader declared = Image::ReadImageHeader(huge);
    EXPECT_TRUE(declared.Valid);
    EXPECT_FALSE(declared.WithinDecoderLimits);
    EXPECT_EQ(declared.Width, 40000u);
    std::filesystem::remove(huge);
}
