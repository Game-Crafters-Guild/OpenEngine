#pragma once

#include <cstdint>

namespace GameEngine::Editor::Startup
{
/// Decides when the hidden main window shows at start-up: after the first present whose
/// Scene View shows an image, or after kFallbackPresents presents when no Scene View
/// shows one (a layout without a Scene View).
class StartupWindowShowGate
{
  public:
    // Covers the first layout solve and a view's first extraction with margin, and keeps a
    // layout without a Scene View hidden for well under a second.
    static constexpr uint32_t kFallbackPresents = 5;

    /// Records one present of the hidden main window. Returns true when the window
    /// must show now.
    bool OnPresent(bool sceneViewShowsImage);

  private:
    uint32_t m_Presents = 0;
};
} // namespace GameEngine::Editor::Startup
