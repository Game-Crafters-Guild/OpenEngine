#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering::Passes
{

/// The scene-linear images a Cube LUT preview renders through the look (and the LUT): one row of
/// RGBA32F texels each, sampled left to right across a preview band.
enum class CubeLutPreviewSource : uint8_t
{
    /// A neutral exposure ramp: mid-gray times 2^-4 at the left edge to 2^+4 at the right.
    Ramp,
    /// Four probes (three saturated hues and a skin tone), each at three exposures, in blocks
    /// separated by black.
    Probes,
};

/// A probe: its name and its color (scene-linear Rec.709). Three are the HDR headroom rig's
/// swatches, the ones the look's hue measurements use: a warm emitter (ember), a saturated blue
/// (sky) and a saturated green (foliage). The fourth is a light skin tone (the color checker's light
/// skin patch, linearized), the color a colorist checks a look LUT on first. Gray shows a LUT's tone
/// and tint; most creative LUTs change hue and skin, which these four show.
struct CubeLutPreviewProbe
{
    std::string_view Name;
    std::array<float, 3> Color;
};

inline constexpr std::array<CubeLutPreviewProbe, 4> kCubeLutPreviewProbes{{
    {"ember", {8.0f, 1.6f, 0.2f}},
    {"sky", {0.8f, 1.6f, 8.0f}},
    {"skin", {0.540f, 0.305f, 0.223f}},
    {"foliage", {1.0f, 6.0f, 0.6f}},
}};

/// The exposures each probe is shown at, in stops from mid-gray luminance (0.18). The top stop is
/// +1, not +2: at +2 the saturated probes' dominant channel already clipped in the Neutral look
/// (the look measured), so that column showed the look's clip rather than the LUT.
inline constexpr std::array<float, 3> kCubeLutPreviewProbeStops{-2.0f, 0.0f, 1.0f};

/// The source's width in texels.
uint32_t CubeLutPreviewSourceWidth(CubeLutPreviewSource source);

/// The source's texels (RGBA32F, one row of CubeLutPreviewSourceWidth texels).
std::vector<float> MakeCubeLutPreviewSourceTexels(CubeLutPreviewSource source);

/// A probe's scene-linear value at one of kCubeLutPreviewProbeStops: its color scaled to a
/// luminance of 0.18 times 2^stop.
std::array<float, 3> CubeLutPreviewProbeValue(size_t probe, size_t stop);

/// The probe source texel at the center of a probe's patch at one stop.
uint32_t CubeLutPreviewProbePatchCenter(size_t probe, size_t stop);

} // namespace GameEngine::Rendering::Passes
