#include "Image/Decode.h"
#include "Image/Encode.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
// Every byte value in every channel, alpha included, over a non-square extent.
std::vector<uint8_t> MakeNoisePattern(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    uint32_t state = 0x9e3779b9u;
    for (uint8_t& byte : rgba)
    {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<uint8_t>(state >> 24);
    }
    return rgba;
}
} // namespace

TEST(ImageEncode, PngRoundTripIsPixelExact)
{
    const std::vector<uint8_t> rgba = MakeNoisePattern(33, 17);
    const std::vector<uint8_t> png = Image::EncodePng(rgba, 33, 17);
    ASSERT_FALSE(png.empty());

    const Image::Image8 decoded = Image::Decode8(png, "round-trip.png", Image::DecodeChannels::Rgba);
    ASSERT_TRUE(decoded.Valid);
    EXPECT_EQ(decoded.Width, 33u);
    EXPECT_EQ(decoded.Height, 17u);
    EXPECT_EQ(decoded.Pixels, rgba);
}

TEST(ImageEncode, PngFileHoldsTheEncodedBytes)
{
    const std::vector<uint8_t> rgba = MakeNoisePattern(9, 5);
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ImageTests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
    ASSERT_TRUE(Image::WritePngFile(path, rgba, 9, 5));

    std::ifstream file(path, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    file.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_EQ(bytes, Image::EncodePng(rgba, 9, 5));
}
