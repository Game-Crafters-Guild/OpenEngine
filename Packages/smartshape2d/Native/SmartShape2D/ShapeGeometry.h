#pragma once

#include "Assets/ModelAsset.h"
#include "SmartShape2D/SmartShapeEdge.h"
#include "Mathematics/Vector2.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::Components { struct SmartShape; }
namespace GameEngine::Spline { class SplineData; }

namespace GameEngine::SmartShape2D
{

/// Material slots match the reflected SmartShape material fields.
enum class ShapePart : uint32
{
    Fill, Top, Right, Bottom, Left, InnerCorner, OuterCorner, StartCap, EndCap, Count
};

struct ShapeGeometry
{
    std::vector<Mesh> Parts = std::vector<Mesh>(static_cast<size_t>(ShapePart::Count));
    std::vector<Components::MaterialRef> Materials;
    std::vector<Mathematics::Vector2> Outline;
    std::string Error;
};

/// Builds local XY geometry. Invalid input returns no triangles and an actionable error.
/// Pure CPU operation; the caller owns the result and may run it off the render thread.
ShapeGeometry BuildShape(const Spline::SplineData& spline, const Components::SmartShape& settings,
                         std::span<const Components::SmartShapeEdge> overrides = {});

} // namespace GameEngine::SmartShape2D
