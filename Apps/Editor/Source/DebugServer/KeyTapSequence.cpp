#include "DebugServer/KeyTapSequence.h"

#include "Input/InputSystem.h"

#include <algorithm>

namespace GameEngine::Editor
{

KeyTapSequence::KeyTapSequence(int count) : m_TapCount(std::clamp(count, 1, kMaxTapCount)) {}

int KeyTapSequence::NextAction()
{
    const bool isPress = m_ActionsSent % 2 == 0;
    ++m_ActionsSent;
    return isPress ? Input::kKeyActionPress : Input::kKeyActionRelease;
}

KeyTapSequence::Step KeyTapSequence::Poll(const SendToWindow& sendToWindow)
{
    if (!sendToWindow(NextAction()))
        return Step::WindowClosed;
    return HasNextAction() ? Step::Pending : Step::Done;
}

} // namespace GameEngine::Editor
