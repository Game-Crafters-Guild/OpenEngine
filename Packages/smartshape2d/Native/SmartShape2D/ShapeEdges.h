#pragma once

#include "SmartShape2D/ShapeGeometry.h"

namespace GameEngine::SmartShape2D
{
void BuildShapeEdges(ShapeGeometry& result, const Components::SmartShape& settings,
    const std::vector<float>& widths, const std::vector<uint32>& segments,
    std::span<const Components::SmartShapeEdge> overrides,
    std::span<const Components::SmartShapeTexture> textures, const Spline::SplineData& spline, float area);
}
