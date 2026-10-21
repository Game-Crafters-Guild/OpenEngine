#include "Rendering/Passes/CubeLutPreviewSources.h"

#include <cmath>

namespace GameEngine::Rendering::Passes
{
namespace
{
constexpr float kMidGray = 0.18f;

// The ramp: 256 texels, four stops either side of mid-gray (card #214).
constexpr uint32_t kRampWidth = 256;
constexpr float kRampStops = 4.0f;

// The probes: a block of 256 texels per probe, its three exposures side by side between black
// margins, so the blocks read apart. Wide enough that a preview band's linear filtering blends only
// the texel at each edge.
constexpr uint32_t kProbeBlockWidth = 256;
constexpr uint32_t kProbeMargin = 8;
constexpr uint32_t kProbePatchWidth =
    (kProbeBlockWidth - 2u * kProbeMargin) / static_cast<uint32_t>(kCubeLutPreviewProbeStops.size());

constexpr std::array<float, 3> kRec709Luma{0.2126f, 0.7152f, 0.0722f};

void SetTexel(std::vector<float>& texels, uint32_t x, const std::array<float, 3>& rgb)
{
    texels[x * 4u + 0u] = rgb[0];
    texels[x * 4u + 1u] = rgb[1];
    texels[x * 4u + 2u] = rgb[2];
    texels[x * 4u + 3u] = 1.0f;
}

uint32_t PatchBegin(size_t probe, size_t stop)
{
    return static_cast<uint32_t>(probe) * kProbeBlockWidth + kProbeMargin + static_cast<uint32_t>(stop) * kProbePatchWidth;
}
} // namespace

uint32_t CubeLutPreviewSourceWidth(CubeLutPreviewSource source)
{
    return source == CubeLutPreviewSource::Ramp ? kRampWidth
                                                : kProbeBlockWidth * static_cast<uint32_t>(kCubeLutPreviewProbes.size());
}

std::array<float, 3> CubeLutPreviewProbeValue(size_t probe, size_t stop)
{
    const std::array<float, 3>& c = kCubeLutPreviewProbes[probe].Color;
    const float luma = c[0] * kRec709Luma[0] + c[1] * kRec709Luma[1] + c[2] * kRec709Luma[2];
    const float scale = kMidGray * std::exp2(kCubeLutPreviewProbeStops[stop]) / luma;
    return {c[0] * scale, c[1] * scale, c[2] * scale};
}

uint32_t CubeLutPreviewProbePatchCenter(size_t probe, size_t stop)
{
    return PatchBegin(probe, stop) + kProbePatchWidth / 2u;
}

std::vector<float> MakeCubeLutPreviewSourceTexels(CubeLutPreviewSource source)
{
    const uint32_t width = CubeLutPreviewSourceWidth(source);
    std::vector<float> texels(static_cast<size_t>(width) * 4u, 0.0f);
    if (source == CubeLutPreviewSource::Ramp)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float t = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const float v = kMidGray * std::exp2((t * 2.0f - 1.0f) * kRampStops);
            SetTexel(texels, x, {v, v, v});
        }
        return texels;
    }
    for (uint32_t x = 0; x < width; ++x)
        texels[x * 4u + 3u] = 1.0f; // black margins, opaque
    for (size_t probe = 0; probe < kCubeLutPreviewProbes.size(); ++probe)
        for (size_t stop = 0; stop < kCubeLutPreviewProbeStops.size(); ++stop)
        {
            const std::array<float, 3> value = CubeLutPreviewProbeValue(probe, stop);
            const uint32_t begin = PatchBegin(probe, stop);
            for (uint32_t x = begin; x < begin + kProbePatchWidth; ++x)
                SetTexel(texels, x, value);
        }
    return texels;
}

} // namespace GameEngine::Rendering::Passes
