#include "DebugServer/ScrollSequence.h"

namespace GameEngine::Editor
{

ScrollSequence::ScrollSequence(int deltaCount, bool waitsForHover)
    : m_DeltaCount(deltaCount), m_HoverFrames(waitsForHover ? 1 : 0)
{
}

ScrollSequence::Step ScrollSequence::Poll(const SendToWindow& sendToWindow)
{
    const int poll = m_Polls++;
    if (poll < m_HoverFrames)
        return Step::Pending;
    if (poll < m_HoverFrames + m_DeltaCount)
        return sendToWindow() ? Step::Pending : Step::WindowClosed;
    return Step::Done;
}

} // namespace GameEngine::Editor
