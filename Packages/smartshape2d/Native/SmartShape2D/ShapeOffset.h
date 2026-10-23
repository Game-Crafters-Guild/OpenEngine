#pragma once

#include "Mathematics/Vector2.h"

#include <span>
#include <vector>

namespace GameEngine::SmartShape2D
{
/// Mitered polygon inflation, with intersection resolution and a miter limit of two.
/// The first resulting contour matches the upstream shape's offset selection.
std::vector<Mathematics::Vector2> OffsetShapePolygon(std::span<const Mathematics::Vector2> outline, float distance);
std::vector<Mathematics::Vector2> OffsetShapePath(std::span<const Mathematics::Vector2> outline, float distance);
/// Butt-ended buffer around an open path, used by open collision shapes.
std::vector<Mathematics::Vector2> BufferShapePath(std::span<const Mathematics::Vector2> outline, float radius);
}
