#pragma once

// A refused declaration. Never compiled: the scanner reads it.

namespace GameEngine::Bad
{

#ifdef _WIN32
/// Shows the console.
// @ge-script
void ShowConsole();
#endif

} // namespace GameEngine::Bad
