#pragma once

// An engine header outside the modules. Never compiled: the scanner reads it.

namespace GameEngine
{

/// Ends the game.
// @ge-script
void RequestExit(std::int32_t code);

} // namespace GameEngine
