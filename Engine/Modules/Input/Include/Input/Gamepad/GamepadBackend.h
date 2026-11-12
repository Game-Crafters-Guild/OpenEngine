#pragma once

#include <cstdint>
#include <memory>

namespace GameEngine::Input
{

struct GamepadFrame;
enum class GamepadRuntimeState : std::uint8_t;

/**
 * @brief One platform's gamepad source, driven by GamepadPoller: Start once on a blocking job, Read
 * on the frame thread after Start returned Available, Stop once after Start and the last Read.
 * Device discovery belongs to Start and the backend's own threads: Read never enumerates.
 */
class GamepadBackend
{
  public:
    virtual ~GamepadBackend() = default;

    /// Creates the platform API and starts device discovery. Runs on a blocking job, never on
    /// the main thread. @return Available when gamepads can be read, otherwise the reason.
    virtual GamepadRuntimeState Start() = 0;

    /// Fills `frame` from the latest device snapshot. Never enumerates devices and never waits.
    virtual void Read(GamepadFrame& frame) = 0;

    /// Stops discovery and releases every device. Also valid when Start never ran.
    virtual void Stop() = 0;
};

/// The backend of this platform: GameInput on Windows, the Game Controller framework on macOS,
/// GLFW's joystick API on Linux and in the browser.
std::unique_ptr<GamepadBackend> CreatePlatformGamepadBackend();

} // namespace GameEngine::Input
