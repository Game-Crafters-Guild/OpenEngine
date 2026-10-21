#pragma once

#include "UI/Controls/Button.h"

#include <functional>

namespace GameEngine
{

struct UIEvent;

/// The AI Assistant's microphone: a Button that can also be held. While the hold
/// predicate is true, a press begins and the release ends, wherever the pointer is
/// then (the button captures the mouse for the hold); a click does nothing. Otherwise
/// it is a plain Button.
class PromptMicButton final : public Button
{
public:
    void SetHold(std::function<bool()> enabled, std::function<void()> begin, std::function<void()> end);

    void OnEvent(UIEvent& event) override;

private:
    std::function<bool()> m_HoldEnabled;
    std::function<void()> m_BeginHold;
    std::function<void()> m_EndHold;
    bool m_Holding = false;
};

} // namespace GameEngine
