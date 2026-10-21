#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

/// A record.
// @ge-script @ge-script-flags
struct Options
{
    bool loop = false;
};

} // namespace GameEngine::Bad
