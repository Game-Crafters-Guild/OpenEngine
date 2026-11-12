#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// A spawn point.
// @ge-script
struct Spawn
{
    Mathematics::WorldPosition at{300000.0, 0.0, 0.0};
};

} // namespace GameEngine::Bad
