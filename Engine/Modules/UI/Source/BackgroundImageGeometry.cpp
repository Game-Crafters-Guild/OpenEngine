#include "BackgroundImageGeometry.h"

#include <algorithm>

namespace GameEngine
{
namespace UI
{

BackgroundImageSize ComputeBackgroundImageSize(const BackgroundImageStyle& bg,
                                               float areaWidth,
                                               float areaHeight,
                                               float contentScale,
                                               float naturalWidth,
                                               float naturalHeight)
{
    const float naturalW = std::max(1.0f, naturalWidth);
    const float naturalH = std::max(1.0f, naturalHeight);
    const float areaW = std::max(1.0f, areaWidth);
    const float areaH = std::max(1.0f, areaHeight);

    BackgroundImageSize size{naturalW, naturalH};
    switch (bg.SizeMode)
    {
    case BackgroundSizeMode::Cover:
    {
        const float s = std::max(areaW / naturalW, areaH / naturalH);
        size = {naturalW * s, naturalH * s};
        break;
    }
    case BackgroundSizeMode::Contain:
    {
        const float s = std::min(areaW / naturalW, areaH / naturalH);
        size = {naturalW * s, naturalH * s};
        break;
    }
    case BackgroundSizeMode::Explicit:
    {
        const bool hasW = bg.SizeX > 0.0f;
        const bool hasH = bg.SizeY > 0.0f;
        const float w = bg.SizeXIsPercent ? bg.SizeX * 0.01f * areaW : bg.SizeX * contentScale;
        const float h = bg.SizeYIsPercent ? bg.SizeY * 0.01f * areaH : bg.SizeY * contentScale;
        if (hasW && hasH)
            size = {w, h};
        else if (hasW)
            size = {w, w * naturalH / naturalW};
        else if (hasH)
            size = {h * naturalW / naturalH, h};
        break;
    }
    case BackgroundSizeMode::Auto:
    {
        const float s = std::min(areaW / naturalW, areaH / naturalH);
        if (s < 1.0f)
            size = {naturalW * s, naturalH * s};
        break;
    }
    }
    size.Width = std::max(1.0f, size.Width);
    size.Height = std::max(1.0f, size.Height);
    return size;
}

} // namespace UI
} // namespace GameEngine
