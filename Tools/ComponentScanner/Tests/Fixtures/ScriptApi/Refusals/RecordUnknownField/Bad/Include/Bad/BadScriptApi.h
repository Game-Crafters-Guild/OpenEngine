#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// Options.
// @ge-script
struct Options
{
    float volume = 1.0f;
    glm::vec3 offset;
};

} // namespace GameEngine::Bad
