#pragma once

// Marks `Hit`, as Beta does. Never compiled: the scanner reads it.

namespace GameEngine::Alpha::Script
{

/// Alpha's hit.
// @ge-script
struct Hit
{
    float distance = 0.0f;
};

/// Takes Alpha's hit.
// @ge-script
void TakeHit(const Hit& hit);

} // namespace GameEngine::Alpha::Script
