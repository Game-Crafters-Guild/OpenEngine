#pragma once

namespace GameEngine
{

class Application;
class EditorDebugServer;

namespace Editor
{
class FramePhaseHistory;
}

// The editor's per-frame phase history: which phase each of the newest frames spent its time
// in, so a slow frame is attributed after the fact.
void RegisterFramePhaseDebugHandlers(EditorDebugServer& server, const Editor::FramePhaseHistory& history,
                                     const Application& app);

} // namespace GameEngine
