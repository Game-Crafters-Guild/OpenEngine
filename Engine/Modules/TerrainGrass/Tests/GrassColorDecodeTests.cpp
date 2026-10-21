// The grass colour contract, executed on the SHIPPED shader code: Tip Color, Root Color and
// Backlight Color are stored as sRGB bytes, and the surface decodes them to linear albedo on read
// (grassColorFromArgb in terrain_grass_surface.glsl, through Includes/srgb_transfer.glsl). Both
// blocks are extracted verbatim at build time (ExtractShaderBlock.cmake) and compiled here through
// GlslShim.h, so an edit to either shader is an edit to what this test measures. The rules built on
// the decode (the automatic root, the default palette) are checked against ColorSRGB in
// TerrainGrassPresetsTests.cpp.

#include <gtest/gtest.h>

#include "GlslShim.h"

#include "Types/Color.h"

#include <algorithm>
#include <cstdint>

namespace
{
namespace Shader
{
using namespace GameEngine::GlslShim;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::pow;
#include "GrassSrgbTransferExtracted.h"
#include "GrassColorDecodeExtracted.h"
} // namespace Shader

struct Hsv
{
    float Saturation; // 0..255
    float Value;      // 0..255
};

Hsv ToHsv255(float r, float g, float b)
{
    const float maxChannel = std::max({r, g, b});
    const float minChannel = std::min({r, g, b});
    const float saturation = maxChannel > 0.0f ? (maxChannel - minChannel) / maxChannel : 0.0f;
    return {saturation * 255.0f, maxChannel * 255.0f};
}

// The default tip and back-light, the swatch of #4345 (179, 219, 77) and the old default back-light,
// the grass-look study's probe colours, a linear-authored game swatch, white and black: dark, mid and
// light greens, a yellow-green and both ends of the range.
constexpr uint32_t kProbeColors[] = {
    0xFF79954Du, 0xFFBCCE61u, 0xFFB3DB4Du, 0xFFE6F06Au, 0xFF3E4F1Au, 0xFF9CD334u,
    0xFF264D0Du, 0xFF193809u, 0xFF637B28u, 0xFFFFFFFFu, 0xFF000000u,
};

TEST(GrassColorDecode, TheSurfaceDecodesTheAuthoredBytesWithTheEnginesSrgbCurve)
{
    for (const uint32_t argb : kProbeColors)
    {
        const Shader::vec3 decoded = Shader::grassColorFromArgb(argb);
        const GameEngine::ColorLinear expected =
            GameEngine::ColorSRGB::FromSRGB255(static_cast<uint8_t>((argb >> 16) & 0xFFu),
                                               static_cast<uint8_t>((argb >> 8) & 0xFFu),
                                               static_cast<uint8_t>(argb & 0xFFu))
                .ToLinear();
        const Hsv got = ToHsv255(decoded.x, decoded.y, decoded.z);
        const Hsv want = ToHsv255(expected.r, expected.g, expected.b);
        EXPECT_NEAR(got.Saturation, want.Saturation, 1.0f) << std::hex << "argb 0x" << argb;
        EXPECT_NEAR(got.Value, want.Value, 1.0f) << std::hex << "argb 0x" << argb;
    }
}

} // namespace
