#pragma once

namespace GameEngine
{

class Application;
class EditorDebugServer;

// The Memory module's stats registry: every TrackedBytes, Arena, ObjectPool and thread's scratch
// arena, with thread names resolved for the scratch entries, the frame and the process's resident
// size.
void RegisterMemoryDebugHandlers(EditorDebugServer& server, const Application& app);

} // namespace GameEngine
