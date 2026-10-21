#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

struct Player
{
    /// The health.
    // @ge-script
    float GetHealth() const;
};

} // namespace GameEngine::Bad
