#pragma once

#include "Types/Color.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine
{
// Shared color conversion helpers that belong in the Types module so they can be
// used without pulling in UI (or any UI-specific dependencies).
namespace ColorUtils
{
/// Pack normalized RGBA channels as 0xAARRGGBB, rounding to the nearest byte.
/// Channels are clamped to [0, 1]; no color-space conversion is performed.
inline std::uint32_t PackArgbRounded(float red, float green, float blue, float alpha)
{
    constexpr float kByteScale = 255.0f;
    constexpr float kRoundToNearest = 0.5f;
    const auto alphaByte = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    const auto redByte = static_cast<std::uint32_t>(std::clamp(red, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    const auto greenByte = static_cast<std::uint32_t>(std::clamp(green, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    const auto blueByte = static_cast<std::uint32_t>(std::clamp(blue, 0.0f, 1.0f) * kByteScale + kRoundToNearest);
    return (alphaByte << 24) | (redByte << 16) | (greenByte << 8) | blueByte;
}

/// Unpack 0xAARRGGBB into normalized RGBA channels in [0, 1]. Inverse of
/// PackArgbRounded; no color-space conversion is performed.
inline ColorLinear UnpackArgb(std::uint32_t argb)
{
    constexpr float kInv255 = 1.0f / 255.0f;
    return {static_cast<float>((argb >> 16) & 0xFF) * kInv255, static_cast<float>((argb >> 8) & 0xFF) * kInv255,
            static_cast<float>(argb & 0xFF) * kInv255, static_cast<float>((argb >> 24) & 0xFF) * kInv255};
}

/// Luminance of a linear Rec.709 / sRGB-primaries color (the engine's scene-linear space), with the
/// BT.709 weights. Relative: 1 for linear white. Alpha is ignored.
constexpr float LinearRec709Luminance(const ColorLinear& linear)
{
    return 0.2126f * linear.r + 0.7152f * linear.g + 0.0722f * linear.b;
}

/// Interprets 0xAARRGGBB as sRGB-encoded bytes (as CSS, the UI and the color picker give it) and decodes it to a
/// linear color, each channel in [0, 1].
inline ColorLinear ArgbToLinear(std::uint32_t argb)
{
    const auto alpha = static_cast<std::uint8_t>((argb >> 24) & 0xFF);
    const auto red = static_cast<std::uint8_t>((argb >> 16) & 0xFF);
    const auto green = static_cast<std::uint8_t>((argb >> 8) & 0xFF);
    const auto blue = static_cast<std::uint8_t>(argb & 0xFF);
    return ColorSRGB::FromSRGB255(red, green, blue, alpha).ToLinear();
}

/// Encodes a linear color to sRGB bytes as 0xFFRRGGBB, each channel clamped to [0, 1]; alpha is ignored and the
/// result is opaque. The inverse of ArgbToLinear for an opaque color.
inline std::uint32_t LinearToArgb(const ColorLinear& linear)
{
    const ColorSRGB srgb = linear.ToSRGB();
    return PackArgbRounded(srgb.r, srgb.g, srgb.b, 1.0f);
}

// Convert HSV to RGB (sRGB space).
// h: hue in degrees (0-360), s: saturation (0-1), v: value (0-1)
// Returns r, g, b in range 0-1
inline void HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    ColorHSV hsv(h, s, v, 1.0f);
    ColorSRGB srgb = hsv.ToSRGB();
    r = srgb.r;
    g = srgb.g;
    b = srgb.b;
}

// Convert RGB to HSV (sRGB space).
// r, g, b: in range 0-1
// Returns h: hue in degrees (0-360), s: saturation (0-1), v: value (0-1)
inline void RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    ColorSRGB srgb(r, g, b, 1.0f);
    ColorHSV hsv = srgb.ToHSV();
    h = hsv.h;
    s = hsv.s;
    v = hsv.v;
}

// Convert HSV to ARGB packed integer (0xAARRGGBB)
inline std::uint32_t HsvToArgb(float h, float s, float v, float a = 1.0f)
{
    ColorHSV hsv(h, s, v, a);
    return hsv.ToARGB();
}

// Convert ARGB packed integer (0xAARRGGBB) to HSV
inline void ArgbToHsv(std::uint32_t argb, float& h, float& s, float& v, float& a)
{
    ColorHSV hsv = ColorHSV::FromARGB(argb);
    h = hsv.h;
    s = hsv.s;
    v = hsv.v;
    a = hsv.a;
}
} // namespace ColorUtils
} // namespace GameEngine
