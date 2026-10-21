#include "Image/Decode.h"
#include "Image/Encode.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
class ImageDecodeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Logger::Log::Config config;
        config.GlobalMinLevel = Logger::LogLevel::Trace;
        Logger::Log::Initialize(config);
        auto sink = std::make_unique<Logger::CallbackSink>();
        sink->RegisterCallback([messages = m_Warnings](const Logger::LogMessage& message)
                               {
                                   if (message.Level == Logger::LogLevel::Warning)
                                       messages->push_back(std::string(message.Message));
                               });
        Logger::Log::AddSink(std::move(sink));
    }

    void TearDown() override { Logger::Log::Shutdown(); }

    std::shared_ptr<std::vector<std::string>> m_Warnings = std::make_shared<std::vector<std::string>>();
};

void WriteBigEndian32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

std::vector<uint8_t> ToBytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

// A binary PGM (magic P5, gray) or PPM (P6, RGB) with 8-bit samples: a header a test
// can write by hand, so it can state every sample.
std::vector<uint8_t> MakePnm8(const char* magic, uint32_t width, uint32_t height, const std::vector<uint8_t>& samples)
{
    std::vector<uint8_t> bytes =
        ToBytes(std::string(magic) + "\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n");
    bytes.insert(bytes.end(), samples.begin(), samples.end());
    return bytes;
}

void AppendChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
    const size_t at = png.size();
    png.resize(at + 4);
    WriteBigEndian32(png, at, static_cast<uint32_t>(data.size()));
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    // stb_image skips chunk CRCs, so the test leaves them zero.
    png.insert(png.end(), 4, 0);
}

// A 16-bit grayscale PNG of one row, its samples big-endian as PNG stores them, in one
// uncompressed deflate block (stb_image does not check the zlib checksum either).
std::vector<uint8_t> MakeGray16Png(const std::vector<uint16_t>& row)
{
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<uint8_t> header(13, 0);
    WriteBigEndian32(header, 0, static_cast<uint32_t>(row.size()));
    WriteBigEndian32(header, 4, 1);
    header[8] = 16; // bit depth; color type 0 (gray)
    AppendChunk(png, "IHDR", header);

    std::vector<uint8_t> scanline = {0}; // filter: none
    for (const uint16_t sample : row)
    {
        scanline.push_back(static_cast<uint8_t>(sample >> 8));
        scanline.push_back(static_cast<uint8_t>(sample));
    }
    const auto length = static_cast<uint16_t>(scanline.size());
    const auto inverse = static_cast<uint16_t>(~length);
    std::vector<uint8_t> zlib = {0x78, 0x01, 0x01, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                                 static_cast<uint8_t>(inverse), static_cast<uint8_t>(inverse >> 8)};
    zlib.insert(zlib.end(), scanline.begin(), scanline.end());
    zlib.insert(zlib.end(), 4, 0);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", {});
    return png;
}
} // namespace

// A PNG whose header declares 25000x25000 grayscale pixels (625 million samples, which
// stb_image's header check accepts) in a file of a few dozen bytes: as RGBA the result
// would be 2.5 GB, past the 2^31 - 1 bytes a decode can produce, so it is refused from
// the header and the warning names the file and the limit.
TEST_F(ImageDecodeTest, RefusesAResultPastTwoGibibytesAndNamesTheFile)
{
    const std::vector<uint8_t> pixel = {10, 20, 30, 255};
    std::vector<uint8_t> png = Image::EncodePng(pixel, 1, 1);
    ASSERT_GT(png.size(), 26u);
    // The IHDR chunk follows the 8-byte signature and its own length and type:
    // width at 16, height at 20, bit depth at 24, color type at 25.
    WriteBigEndian32(png, 16, 25000);
    WriteBigEndian32(png, 20, 25000);
    png[25] = 0; // grayscale

    const Image::Image8 decoded = Image::Decode8(png, "Textures/huge-height.png", Image::DecodeChannels::Rgba);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    EXPECT_TRUE(decoded.Pixels.empty());
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Textures/huge-height.png"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("25000x25000"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("2147483647 bytes"), std::string::npos) << m_Warnings->front();
}

// stb_image's PNG header reader refuses a PNG holding more than 2^30 samples before the
// decode sees its size: an RGBA PNG of 16385x16385 is refused with a warning that names
// the file and that limit, not dropped as "not an image".
TEST_F(ImageDecodeTest, RefusesAPngPastTheSampleLimitOfItsHeaderAndNamesTheFile)
{
    const std::vector<uint8_t> pixel = {10, 20, 30, 255};
    std::vector<uint8_t> png = Image::EncodePng(pixel, 1, 1);
    WriteBigEndian32(png, 16, 16385);
    WriteBigEndian32(png, 20, 16385);

    const Image::Image8 decoded = Image::Decode8(png, "Textures/huge-albedo.png", Image::DecodeChannels::Rgba);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Textures/huge-albedo.png"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("16385x16385"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("1073741824 samples"), std::string::npos) << m_Warnings->front();
}

// A 16000x16000 HDR is 3 GB as RGB floats: refused from the header, by name.
TEST_F(ImageDecodeTest, DecodeFloatRefusesAnHdrPastTwoGibibytesAndNamesTheFile)
{
    std::vector<uint8_t> hdr = ToBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 16000 +X 16000\n");
    hdr.insert(hdr.end(), {128, 64, 32, 129});

    const Image::ImageFloat decoded = Image::DecodeFloat(hdr, "Sky/huge.hdr", Image::DecodeChannels::FromFile);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Sky/huge.hdr"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("2147483647 bytes"), std::string::npos) << m_Warnings->front();
}

// An image whose header reads but whose data does not decode (a PNG cut off inside its
// image data) warns once with the file's name and stb_image's reason.
TEST_F(ImageDecodeTest, WarnsWithTheReasonWhenTheImageDataDoesNotDecode)
{
    std::vector<uint8_t> rgba(32u * 32u * 4u);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = static_cast<uint8_t>(i * 2654435761u >> 24);
    std::vector<uint8_t> png = Image::EncodePng(rgba, 32, 32);
    ASSERT_GT(png.size(), 200u);
    png.resize(png.size() / 2);

    const Image::Image8 decoded = Image::Decode8(png, "Textures/cut-off.png", Image::DecodeChannels::Rgba);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    EXPECT_TRUE(decoded.Pixels.empty());
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Textures/cut-off.png"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("could not be decoded: "), std::string::npos) << m_Warnings->front();
}

// FromFile keeps a gray file gray (one channel, as R8 textures need); a request for
// other channels converts: gray expands to opaque RGBA, RGB reduces to luma.
TEST_F(ImageDecodeTest, Decode8KeepsTheFilesChannelsOrConvertsToTheRequestedOnes)
{
    const std::vector<uint8_t> gray = MakePnm8("P5", 2, 1, {10, 200});
    const Image::Image8 asFile = Image::Decode8(gray, "gray.pgm", Image::DecodeChannels::FromFile);
    ASSERT_TRUE(asFile.Valid);
    EXPECT_EQ(asFile.Channels, 1u);
    EXPECT_EQ(asFile.Pixels, (std::vector<uint8_t>{10, 200}));

    const Image::Image8 asRgba = Image::Decode8(gray, "gray.pgm", Image::DecodeChannels::Rgba);
    ASSERT_TRUE(asRgba.Valid);
    EXPECT_EQ(asRgba.Channels, 4u);
    EXPECT_EQ(asRgba.Pixels, (std::vector<uint8_t>{10, 10, 10, 255, 200, 200, 200, 255}));

    const std::vector<uint8_t> rgb = MakePnm8("P6", 1, 1, {100, 150, 50});
    const Image::Image8 asRgb = Image::Decode8(rgb, "rgb.ppm", Image::DecodeChannels::Rgb);
    ASSERT_TRUE(asRgb.Valid);
    EXPECT_EQ(asRgb.Pixels, (std::vector<uint8_t>{100, 150, 50}));
    const Image::Image8 asGray = Image::Decode8(rgb, "rgb.ppm", Image::DecodeChannels::Gray);
    ASSERT_TRUE(asGray.Valid);
    EXPECT_EQ(asGray.Channels, 1u);
    // Luma: (77 R + 150 G + 29 B) / 256.
    EXPECT_EQ(asGray.Pixels, (std::vector<uint8_t>{(77 * 100 + 150 * 150 + 29 * 50) >> 8}));
    Logger::Log::Flush();
    EXPECT_TRUE(m_Warnings->empty());
}

// A heightmap keeps all 16 bits; an 8-bit file widens so that 255 reads as 65535.
TEST_F(ImageDecodeTest, Decode16KeepsSixteenBitSamplesAndWidensEightBitOnes)
{
    const Image::Image16 height =
        Image::Decode16(MakeGray16Png({0x0000, 0x1234, 0xfedc}), "height.png", Image::DecodeChannels::Gray);
    ASSERT_TRUE(height.Valid);
    EXPECT_EQ(height.Width, 3u);
    EXPECT_EQ(height.Height, 1u);
    EXPECT_EQ(height.Channels, 1u);
    EXPECT_EQ(height.Pixels, (std::vector<uint16_t>{0x0000, 0x1234, 0xfedc}));

    const Image::Image16 widened =
        Image::Decode16(MakePnm8("P5", 2, 1, {200, 255}), "gray.pgm", Image::DecodeChannels::Gray);
    ASSERT_TRUE(widened.Valid);
    EXPECT_EQ(widened.Pixels, (std::vector<uint16_t>{200 * 257, 65535}));
}

// A 20000x20000 grayscale header (400 million samples, which stb_image accepts) decoded
// to 16-bit RGBA would be 3.2 GB: refused from the header with a warning that names the
// file and the limit.
TEST_F(ImageDecodeTest, Decode16RefusesAResultPastTwoGibibytesAndNamesTheFile)
{
    std::vector<uint8_t> png = MakeGray16Png({1, 2});
    WriteBigEndian32(png, 16, 20000);
    WriteBigEndian32(png, 20, 20000);

    const Image::Image16 decoded = Image::Decode16(png, "Terrain/huge-height.png", Image::DecodeChannels::Rgba);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    EXPECT_TRUE(decoded.Pixels.empty());
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Terrain/huge-height.png"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("2147483647 bytes"), std::string::npos) << m_Warnings->front();
}

// An HDR file keeps its values (RGBE: mantissa * 2^(exponent - 136)); an 8-bit file is
// linearized with gamma 2.2.
TEST_F(ImageDecodeTest, DecodeFloatKeepsHdrValuesAndLinearizesEightBitColor)
{
    std::vector<uint8_t> hdr = ToBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n");
    hdr.insert(hdr.end(), {128, 64, 32, 129, 255, 0, 0, 140});
    const Image::ImageFloat asFile = Image::DecodeFloat(hdr, "sky.hdr", Image::DecodeChannels::FromFile);
    ASSERT_TRUE(asFile.Valid);
    EXPECT_EQ(asFile.Channels, 3u);
    EXPECT_EQ(asFile.Pixels, (std::vector<float>{1.0f, 0.5f, 0.25f, 4080.0f, 0.0f, 0.0f}));
    const Image::ImageFloat asRgba = Image::DecodeFloat(hdr, "sky.hdr", Image::DecodeChannels::Rgba);
    ASSERT_TRUE(asRgba.Valid);
    EXPECT_EQ(asRgba.Pixels, (std::vector<float>{1.0f, 0.5f, 0.25f, 1.0f, 4080.0f, 0.0f, 0.0f, 1.0f}));

    const Image::ImageFloat gray =
        Image::DecodeFloat(MakePnm8("P5", 2, 1, {128, 255}), "gray.pgm", Image::DecodeChannels::Gray);
    ASSERT_TRUE(gray.Valid);
    ASSERT_EQ(gray.Pixels.size(), 2u);
    EXPECT_NEAR(gray.Pixels[0], std::pow(128.0f / 255.0f, 2.2f), 1e-6f);
    EXPECT_FLOAT_EQ(gray.Pixels[1], 1.0f);
}

// stb_image refuses a PNG side past 2^24 pixels: a 16777217 x 1 PNG is refused by name.
TEST_F(ImageDecodeTest, RefusesAPngPastTheSideLimitAndNamesTheFile)
{
    const std::vector<uint8_t> pixel = {10, 20, 30, 255};
    std::vector<uint8_t> png = Image::EncodePng(pixel, 1, 1);
    WriteBigEndian32(png, 16, 16777217);

    const Image::Image8 decoded = Image::Decode8(png, "Textures/long.png", Image::DecodeChannels::Rgba);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Textures/long.png"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("16777216 pixels a side"), std::string::npos) << m_Warnings->front();
}

// A 16-bit grayscale PNG of 32768 x 32767 holds 2^30 - 32768 samples, but its image rows
// (65536 bytes each, plus one) pass 2^31 - 1 bytes, which stb_image's PNG decoder refuses:
// the decode is refused from the header, by name.
TEST_F(ImageDecodeTest, Decode16RefusesAPngWhoseRowsPassTwoGibibytesAndNamesTheFile)
{
    std::vector<uint8_t> png = MakeGray16Png({1, 2});
    WriteBigEndian32(png, 16, 32768);
    WriteBigEndian32(png, 20, 32767);

    const Image::Image16 decoded = Image::Decode16(png, "Terrain/height-32768.png", Image::DecodeChannels::Gray);
    Logger::Log::Flush();
    EXPECT_FALSE(decoded.Valid);
    ASSERT_EQ(m_Warnings->size(), 1u);
    EXPECT_NE(m_Warnings->front().find("Terrain/height-32768.png"), std::string::npos) << m_Warnings->front();
    EXPECT_NE(m_Warnings->front().find("2147483647 bytes of image rows"), std::string::npos) << m_Warnings->front();
}
