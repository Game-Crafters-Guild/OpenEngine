#pragma once

#include <cstdint>
#include <functional>

namespace GameEngine::Editor
{

/// The key events of a send_key tap repeated `count` times, one event a frame: a press on one
/// frame and its release on the next. Input keeps one press flag for each key in a frame, so a
/// game sees each tap as its own press only when no two presses share a frame.
class KeyTapSequence
{
public:
    /// What one poll did: the sequence continues, it ended, or the window closed before it ended.
    enum class Step : uint8_t
    {
        Pending,
        Done,
        WindowClosed,
    };

    /// Sends one key action (Input::kKeyActionPress or Input::kKeyActionRelease) to the window.
    /// It finds the window again on each call and gives false when the window is closed.
    using SendToWindow = std::function<bool(int action)>;

    /// `count` is clamped to 1 to kMaxTapCount.
    explicit KeyTapSequence(int count);

    /// Sends this frame's key action through `sendToWindow`. Call it once a frame until it gives
    /// Done or WindowClosed.
    Step Poll(const SendToWindow& sendToWindow);

    /// The frames the sequence takes: a press frame and a release frame for each tap.
    int FrameCount() const { return 2 * m_TapCount; }

    int TapCount() const { return m_TapCount; }

    static constexpr int kMaxTapCount = 64;

private:
    int NextAction();
    bool HasNextAction() const { return m_ActionsSent < 2 * m_TapCount; }

    int m_TapCount = 1;
    int m_ActionsSent = 0;
};

} // namespace GameEngine::Editor
