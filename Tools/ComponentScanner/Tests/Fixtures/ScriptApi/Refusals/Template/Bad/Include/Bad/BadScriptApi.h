#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// Clamps a value.
// @ge-script
template <typename T>
T Clamp(T value);

} // namespace GameEngine::Bad
