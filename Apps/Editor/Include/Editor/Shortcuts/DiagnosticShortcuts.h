#pragma once

#include <functional>
#include <string>

namespace GameEngine::Input
{
class InputSystem;
}

namespace GameEngine::Editor
{

/// The editor's diagnostic hotkeys: the CPU profiler (F2 enables it, F2 again dumps the
/// last frame, Shift+F2 disables it; only when CPU profiling is compiled in) and the UI
/// Demo panel (F3). They are Editor.Global actions in the Shortcuts catalog, not reserved
/// for the host, so the window router offers the key to a game running in a focused Game
/// View first: a game that binds or polls F2 or F3 takes it, and the editor acts only on
/// what reaches its InputSystem.
class DiagnosticShortcuts
{
  public:
    /// @param perfDumpLine  The perf breakdown a CPU profiler dump carries.
    /// @param showUiDemo    Shows the UI Demo panel, or activates it when it is open.
    DiagnosticShortcuts(std::function<std::string()> perfDumpLine, std::function<void()> showUiDemo);

    /// Acts on the diagnostic actions `input` triggered this frame. Called once per frame,
    /// with the other editor actions.
    void Update(const Input::InputSystem& input) const;

  private:
    std::function<std::string()> m_PerfDumpLine;
    std::function<void()> m_ShowUiDemo;
};

} // namespace GameEngine::Editor
