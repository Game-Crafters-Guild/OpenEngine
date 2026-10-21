#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Navigation's live state: what the path solve budget did in the newest frame and in total, and a
// path solved on demand across a baked NavigationSurface.
void RegisterNavigationDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
