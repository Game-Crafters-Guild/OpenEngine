#pragma once

// The Application script API: what a game asks of the application that runs it. Every marked
// declaration is generated into the C# binding (Application); a native script calls the same
// functions. The bodies are in Engine (Engine/Source/Core/Application.cpp), beside the
// application they ask.

#include <cstdint>

namespace GameEngine::Platform::Script
{

/// Ends the game after the current frame. A standalone game runs its normal shutdown, scripts'
/// OnDestroy included, and the process exits with exitCode; a later request replaces the code. In
/// the editor, play stops the way the Stop button stops it, change review included; the editor
/// stays open and the code is unused. With no application, or in the editor outside play, the
/// call does nothing.
// @ge-script
void RequestExit(std::int32_t exitCode = 0);

} // namespace GameEngine::Platform::Script
