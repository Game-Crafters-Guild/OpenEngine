#pragma once

// A module with no script API header. Prose that names the @ge-script marker is not a
// marker, and a component field marker is not a script marker, so this header is not refused.

namespace GameEngine::Plain
{
int Unbound();

struct PlainSettings
{
    float Speed = 1.0f; // @ge-tooltip How fast Plain moves.
};
}
