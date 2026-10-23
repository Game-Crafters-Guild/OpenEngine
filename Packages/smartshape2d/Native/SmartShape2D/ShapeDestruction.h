#pragma once

#include "Mathematics/Vector2.h"

#include <span>
#include <vector>

namespace GameEngine { struct Mesh; }
namespace GameEngine::Components { struct SmartShapeCut; }

namespace GameEngine::SmartShape2D
{
/// A convex, counterclockwise cut in the shape's local XY plane.
using ShapeCut = std::vector<Mathematics::Vector2>;

std::vector<ShapeCut> BuildShapeCuts(std::span<const Components::SmartShapeCut> cuts);

/// Removes cuts from a planar mesh while retaining its material UVs and winding.
/// Empty cuts leave the mesh unchanged. Call before registering GPU or collision data.
void CutShapeMesh(Mesh& mesh, std::span<const ShapeCut> cuts);
}
