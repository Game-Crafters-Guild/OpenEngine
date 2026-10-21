#include "SvgBackgroundRaster.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace UI
{

namespace
{
// A drawn edge this close above a whole pixel is that pixel: products of CSS
// px, the content scale and fit ratios carry float error, never a real
// fraction this small.
constexpr float kWholePixelTolerance = 1.0f / 256.0f;

uint32_t RasterEdge(float drawnPx)
{
    if (!std::isfinite(drawnPx) || drawnPx <= 1.0f)
        return 1u;
    const float clamped = std::min(drawnPx, static_cast<float>(kMaxSvgBackgroundRasterEdge));
    return std::max(1u, static_cast<uint32_t>(std::ceil(clamped - kWholePixelTolerance)));
}
} // namespace

SvgRasterSize SvgBackgroundRasterSize(const BackgroundImageSize& drawn)
{
    return {RasterEdge(drawn.Width), RasterEdge(drawn.Height)};
}

bool IsSvgRasterSizeCloseEnough(SvgRasterSize baked, SvgRasterSize wanted)
{
    const auto within = [](uint32_t a, uint32_t b) { return a <= 2u * b && b <= 2u * a; };
    return within(baked.Width, wanted.Width) && within(baked.Height, wanted.Height);
}

size_t NearestSvgBackgroundVariant(const std::vector<SvgBackgroundVariant>& variants, SvgRasterSize size)
{
    size_t best = variants.size();
    float bestDistance = 0.0f;
    const float targetW = std::log(static_cast<float>(std::max(1u, size.Width)));
    const float targetH = std::log(static_cast<float>(std::max(1u, size.Height)));
    for (size_t i = 0; i < variants.size(); ++i)
    {
        if (!variants[i].Handle.IsValid())
            continue;
        const float distance =
            std::abs(std::log(static_cast<float>(std::max(1u, variants[i].Size.Width))) - targetW) +
            std::abs(std::log(static_cast<float>(std::max(1u, variants[i].Size.Height))) - targetH);
        if (best == variants.size() || distance < bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace UI
} // namespace GameEngine
