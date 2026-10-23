#pragma once

#include "Physics/PhysicsShapes.h"

namespace GameEngine::SmartShape2D
{
struct ShapeGeometry;
/// Extrudes a closed fill or an open edge ribbon into a static triangle collider.
Physics::MeshShapeDef BuildCollision(const ShapeGeometry& geometry, bool closed, float depth);
}
