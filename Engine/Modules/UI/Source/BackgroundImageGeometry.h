#pragma once

#include "UI/UIStyle.h"

namespace GameEngine
{
namespace UI
{

// The size a background image is drawn at, in physical px.
struct BackgroundImageSize
{
    float Width = 0.0f;
    float Height = 0.0f;
};

// The drawn size of a background image whose natural size is naturalWidth x
// naturalHeight physical px, under `bg`, in a positioning area (the padding
// box) of areaWidth x areaHeight physical px at `contentScale` physical px per
// CSS px. The background-size rules: Cover and Contain fit the natural aspect
// to the area; an Explicit length is CSS px x contentScale and a percentage is
// of the area, and an axis left auto keeps the natural aspect; Auto is the
// natural size, shrunk to fit the area. At least 1 px on each axis. The draw
// and an SVG's raster size both come from here, so a raster baked for a draw
// is the size that draw paints.
BackgroundImageSize ComputeBackgroundImageSize(const BackgroundImageStyle& bg,
                                               float areaWidth,
                                               float areaHeight,
                                               float contentScale,
                                               float naturalWidth,
                                               float naturalHeight);

} // namespace UI
} // namespace GameEngine
