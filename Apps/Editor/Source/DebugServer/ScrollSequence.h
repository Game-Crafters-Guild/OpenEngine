#pragma once

#include <cstdint>
#include <functional>

namespace GameEngine::Editor
{

/// The polls of a send_scroll wheel: one wheel delta on each of `count` frames, then the reply.
/// A request that moves the pointer first waits one frame, because the UI scrolls the element
/// that its last Update found under the pointer.
class ScrollSequence
{
public:
    /// What one poll did: the sequence continues, it ended, or the window closed before it ended.
    enum class Step : uint8_t
    {
        Pending,
        Done,
        WindowClosed,
    };

    /// Sends one wheel delta to the window. It finds the window again on each call and gives
    /// false when the window is closed.
    using SendToWindow = std::function<bool()>;

    /// `deltaCount` is 1 to kMaxDeltaCount; the request handler refuses other counts.
    ScrollSequence(int deltaCount, bool waitsForHover);

    /// Sends this frame's delta through `sendToWindow` when this frame has one. Call it once a
    /// frame until it gives Done or WindowClosed.
    Step Poll(const SendToWindow& sendToWindow);

    /// The polls the sequence takes: the hover frame, one frame for each delta and the reply frame.
    int FrameCount() const { return m_HoverFrames + m_DeltaCount + 1; }

    int DeltaCount() const { return m_DeltaCount; }

    static constexpr int kMaxDeltaCount = 240;

private:
    int m_DeltaCount = 1;
    int m_HoverFrames = 0;
    int m_Polls = 0;
};

} // namespace GameEngine::Editor
