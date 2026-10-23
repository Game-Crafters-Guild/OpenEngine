#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// Every entity near a point.
// @ge-script
void FindNear(Mathematics::Vector3 point, std::span<ECS::EntityHandle> entities);

} // namespace GameEngine::Bad
