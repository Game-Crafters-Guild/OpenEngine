#pragma once

// Marks `Hit`, as Alpha does. Never compiled: the scanner reads it.

namespace GameEngine::Beta::Script
{

/// Beta's hit.
// @ge-script
struct Hit
{
    Mathematics::Vector3 point;
    float distance = 0.0f;
};

/// Uses Beta's hit.
// @ge-script
void UseHit(const Hit& hit);

} // namespace GameEngine::Beta::Script
