#pragma once

#include "Physics/PhysicsShapes.h"

namespace GameEngine::Components { struct SmartShape; }

namespace GameEngine::SmartShape2D
{
struct ShapeGeometry;
/// Extrudes a closed fill or an open edge ribbon into a static triangle collider.
Physics::MeshShapeDef BuildCollision(const ShapeGeometry& geometry, bool closed, float depth);
Physics::MeshShapeDef BuildCollision(const ShapeGeometry& geometry, bool closed, const Components::SmartShape& settings);
}
